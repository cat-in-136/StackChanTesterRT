#include "udp_audio.h"

#include <M5Unified.h>
#include <WiFi.h>

#include <cstring>

namespace {
const uint16_t kLocalPort = 54321;
const size_t kHeaderSize = 16;
const int kMaxDatagramsPerLoop = 4;

uint16_t readU16BE(const uint8_t* p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
uint32_t readU32BE(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
void writeU16BE(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xff);
}
void writeU32BE(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)((v >> 16) & 0xff);
  p[2] = (uint8_t)((v >> 8) & 0xff);
  p[3] = (uint8_t)(v & 0xff);
}
}  // namespace

UdpAudio::UdpAudio() { mbedtls_aes_init(&aes_); }

UdpAudio::~UdpAudio() {
  end();
  mbedtls_aes_free(&aes_);
}

bool UdpAudio::hexToBytes(const std::string& hex, uint8_t* out,
                          size_t out_len) {
  if (hex.size() < out_len * 2) return false;
  for (size_t i = 0; i < out_len; i++) {
    char c1 = hex[i * 2], c2 = hex[i * 2 + 1];
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int h = nib(c1), l = nib(c2);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

bool UdpAudio::begin(const std::string& server, int port,
                     const std::string& keyHex,
                     const std::string& nonceHex) {
  end();
  if (server.empty() || port <= 0) {
    M5_LOGE("udp: bad endpoint");
    return false;
  }
  if (!hexToBytes(keyHex, key_, sizeof(key_))) {
    M5_LOGE("udp: bad key hex len=%u", (unsigned)keyHex.size());
    return false;
  }
  if (!hexToBytes(nonceHex, base_nonce_, sizeof(base_nonce_))) {
    M5_LOGE("udp: bad nonce hex len=%u", (unsigned)nonceHex.size());
    return false;
  }
  if (!WiFi.hostByName(server.c_str(), remote_ip_)) {
    M5_LOGE("udp: dns failed %s", server.c_str());
    return false;
  }
  remote_port_ = (uint16_t)port;
  if (udp_.begin(kLocalPort) != 1) {
    M5_LOGE("udp: bind %u failed", (unsigned)kLocalPort);
    return false;
  }
  if (mbedtls_aes_setkey_enc(&aes_, key_, 128) != 0) {
    M5_LOGE("udp: aes key setup failed");
    udp_.stop();
    return false;
  }
  aes_ready_ = true;
  remote_seq_ = 0;
  local_seq_ = 0;
  rx_count_ = 0;
  drop_count_ = 0;
  tx_count_ = 0;
  logged_packets_ = 0;
  logged_remote_ = false;
  active_ = true;
  M5_LOGI("udp: open %s:%d local=%u", server.c_str(), port,
          (unsigned)kLocalPort);
  Serial.printf("CHAT UDP-OPEN %s:%d\r\n", server.c_str(), port);
  return true;
}

void UdpAudio::end() {
  if (active_) {
    udp_.stop();
    active_ = false;
  }
  aes_ready_ = false;
}

bool UdpAudio::crypt(const uint8_t* in, uint8_t* out, size_t len,
                     const uint8_t* iv) {
  if (!aes_ready_) return false;
  uint8_t counter[16];
  uint8_t stream[16] = {0};
  size_t off = 0;
  memcpy(counter, iv, sizeof(counter));
  // mbedTLS の CTR は in/out 同一バッファ可だが、ここでは分離して呼ぶ。
  return mbedtls_aes_crypt_ctr(&aes_, len, &off, counter, stream, in, out) ==
         0;
}

bool UdpAudio::sendDatagram(const uint8_t* payload, size_t len,
                            uint32_t timestamp) {
  if (!active_ || !aes_ready_) return false;
  if (len > 1500) return false;
  // 公式 SendAudio と同一組立: base nonce + len/ts/seq overlay。
  uint8_t hdr[kHeaderSize];
  memcpy(hdr, base_nonce_, sizeof(hdr));
  writeU16BE(hdr + 2, (uint16_t)len);
  writeU32BE(hdr + 8, timestamp);
  writeU32BE(hdr + 12, ++local_seq_);
  uint8_t enc[1600];
  if (len > 0 && !crypt(payload, enc, len, hdr)) return false;
  if (udp_.beginPacket(remote_ip_, remote_port_) != 1) return false;
  udp_.write(hdr, sizeof(hdr));
  if (len > 0) udp_.write(enc, len);
  if (udp_.endPacket() != 1) return false;
  tx_count_++;
  return true;
}

bool UdpAudio::punch(const uint8_t* payload, size_t len, uint32_t now_ms) {
  // NAT マッピング確立 + server への endpoint 学習用。mic-less のため必須。
  // (server は最初の上り datagram の source を下り送出先にする)
  bool ok = sendDatagram(payload, len, now_ms);
  M5_LOGI("udp: punch len=%u ok=%d", (unsigned)len, (int)ok);
  return ok;
}

void UdpAudio::loop(const AudioCb& cb, uint32_t now_ms) {
  (void)now_ms;
  if (!active_) return;
  for (int i = 0; i < kMaxDatagramsPerLoop; i++) {
    int n = udp_.parsePacket();
    if (n <= 0) break;
    if (!logged_remote_) {
      logged_remote_ = true;
      M5_LOGI("udp: first rx from %s:%d", udp_.remoteIP().toString().c_str(),
              udp_.remotePort());
    }
    if ((size_t)n > sizeof(rx_buf_)) {
      drop_count_++;
      // 読み捨てて次へ。
      while (n > 0) {
        int r = udp_.read(rx_buf_, sizeof(rx_buf_));
        if (r <= 0) break;
        n -= r;
      }
      continue;
    }
    int got = udp_.read(rx_buf_, n);
    if (got != n) {
      drop_count_++;
      continue;
    }
    if ((size_t)got < kHeaderSize || rx_buf_[0] != 0x01) {
      drop_count_++;
      M5_LOGW("udp: bad header size=%d type=%02x", got, rx_buf_[0]);
      continue;
    }
    uint16_t payload_len = readU16BE(rx_buf_ + 2);
    uint32_t timestamp = readU32BE(rx_buf_ + 8);
    uint32_t seq = readU32BE(rx_buf_ + 12);
    if ((size_t)got != kHeaderSize + payload_len) {
      drop_count_++;
      M5_LOGW("udp: len mismatch hdr=%u dgram=%d", (unsigned)payload_len,
              got);
      continue;
    }
    if (seq <= remote_seq_) {
      drop_count_++;
      continue;  // replay/duplicate は黙って破棄 (公式と同様)
    }
    bool gap = (remote_seq_ != 0 && seq != remote_seq_ + 1);
    if (gap) {
      M5_LOGW("udp: seq gap %lu -> %lu", (unsigned long)remote_seq_,
              (unsigned long)seq);
    }
    remote_seq_ = seq;
    if (payload_len > sizeof(plain_buf_)) {
      drop_count_++;
      continue;
    }
    if (!crypt(rx_buf_ + kHeaderSize, plain_buf_, payload_len, rx_buf_)) {
      drop_count_++;
      M5_LOGW("udp: decrypt failed seq=%lu", (unsigned long)seq);
      continue;
    }
    rx_count_++;
    if (logged_packets_ < 3 || (rx_count_ % 100) == 0) {
      logged_packets_++;
      M5_LOGI("udp: rx seq=%lu ts=%lu len=%u", (unsigned long)seq,
              (unsigned long)timestamp, (unsigned)payload_len);
      Serial.printf("CHAT UDP-RX seq=%lu len=%u\r\n", (unsigned long)seq,
                    (unsigned)payload_len);
    }
    if (cb) cb(plain_buf_, payload_len, timestamp, gap);
  }
}
