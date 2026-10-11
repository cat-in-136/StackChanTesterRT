#pragma once
// UdpAudio: xiaozhi UDP音声下り (MQTT hello応答の udp{server,port,key,nonce} 用)。
// 16Bヘッダ検証・seq検証・AES-128-CTR復号までを担当し、復号済み生Opusを
// コールバックへ渡す。Opus再生自体は OpusPlayer 側。
// 参照: /references/xiaozhi-esp32/main/protocols/mqtt_protocol.cc:302-380 (RX),
//       同:204-247 (TX/nonce組立)、docs/mqtt-udp.md §4。
#include <WiFiUdp.h>
#include <mbedtls/aes.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class UdpAudio {
 public:
  // gap=true: seq飛び (欠落あり)。再生側はPLC等で補うか単に次へ進む。
  using AudioCb = std::function<void(const uint8_t* opus, size_t len,
                                     uint32_t timestamp, bool gap)>;
  UdpAudio();
  ~UdpAudio();

  // keyHex/nonceHex は server hello の hex128。そのまま保持し復号に使う。
  bool begin(const std::string& server, int port,
             const std::string& keyHex, const std::string& nonceHex);
  void end();
  bool active() const { return active_; }

  // 受信ポーリング (mqtt_.loop() と同様に ChatbotRuntime::loop から呼ぶ)。
  // 1回で最大4 datagram 処理する。
  void loop(const AudioCb& cb, uint32_t now_ms);

  // NAT punch 用の最小送信。payload は呼出側が用意。
  // len=0 (header のみ keepalive) 可: endpoint学習 + NAT維持が目的。
  // header = base nonce + len/ts/seq overlay、AES-CTR 後に送信。
  bool punch(const uint8_t* payload, size_t len, uint32_t now_ms);

  uint32_t rxCount() const { return rx_count_; }
  uint32_t dropCount() const { return drop_count_; }
  uint32_t txCount() const { return tx_count_; }

 private:
  static bool hexToBytes(const std::string& hex, uint8_t* out, size_t out_len);
  bool crypt(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv);
  bool sendDatagram(const uint8_t* payload, size_t len, uint32_t timestamp);

  WiFiUDP udp_;
  bool active_ = false;
  IPAddress remote_ip_;
  uint16_t remote_port_ = 0;

  uint8_t key_[16] = {0};
  uint8_t base_nonce_[16] = {0};
  mbedtls_aes_context aes_;
  bool aes_ready_ = false;

  uint32_t remote_seq_ = 0;
  uint32_t local_seq_ = 0;
  uint32_t rx_count_ = 0;
  uint32_t drop_count_ = 0;
  uint32_t tx_count_ = 0;
  uint32_t logged_packets_ = 0;
  bool logged_remote_ = false;

  static const size_t kBufSize = 2048;
  uint8_t rx_buf_[kBufSize];
  uint8_t plain_buf_[kBufSize];
};
