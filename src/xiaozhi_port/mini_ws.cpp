#include "mini_ws.h"
#include "tls_bundle.h"

#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

// 内部クライアント実装 (TLS/平文の切替)
namespace {
class SecureCli : public MiniWs::Client {
 public:
  bool connect(const char* host, int port) override {
    applyTlsBundle(c_);  // 正規 CA 検証 (既定)。ws:// は PlainCli 側。
    c_.setTimeout(10000);
    return c_.connect(host, port) == 1;
  }
  void stop() override { c_.stop(); }
  bool connected() override { return c_.connected(); }
  int available() override { return c_.available(); }
  int read(uint8_t* buf, size_t n) override { return c_.read(buf, n); }
  size_t write(const uint8_t* buf, size_t n) override {
    return c_.write(buf, n);
  }
  void setTimeout(int ms) override { c_.setTimeout(ms); }

 private:
  WiFiClientSecure c_;
};
class PlainCli : public MiniWs::Client {
 public:
  bool connect(const char* host, int port) override {
    c_.setTimeout(10000);
    return c_.connect(host, port) == 1;
  }
  void stop() override { c_.stop(); }
  bool connected() override { return c_.connected(); }
  int available() override { return c_.available(); }
  int read(uint8_t* buf, size_t n) override { return c_.read(buf, n); }
  size_t write(const uint8_t* buf, size_t n) override {
    return c_.write(buf, n);
  }
  void setTimeout(int ms) override { c_.setTimeout(ms); }

 private:
  WiFiClient c_;
};

std::string base64enc(const uint8_t* in, size_t len) {
  size_t out_len = 0;
  mbedtls_base64_encode(nullptr, 0, &out_len, in, len);
  std::string o;
  o.resize(out_len);
  size_t written = 0;
  if (mbedtls_base64_encode((unsigned char*)o.data(), o.size(), &written, in,
                             len) != 0)
    return "";
  // mbedtls は NUL 終端を含めて数えるため末尾調整
  while (!o.empty() && o.back() == '\0') o.pop_back();
  o.resize(written);
  return o;
}

std::string wsAccept(const std::string& key) {
  static const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string s = key + guid;
  uint8_t digest[20];
  mbedtls_sha1((const unsigned char*)s.data(), s.size(), digest);
  return base64enc(digest, sizeof(digest));
}

std::string randKey() {
  uint8_t raw[16];
  for (int i = 0; i < 16; i++) raw[i] = (uint8_t)(esp_random() & 0xff);
  return base64enc(raw, sizeof(raw));
}
}  // namespace

MiniWs::MiniWs() {}
MiniWs::~MiniWs() { close(); }

bool MiniWs::parseUrl(const std::string& url, bool& tls, std::string& host,
                      int& port, std::string& path) {
  bool wss = url.rfind("wss://", 0) == 0;
  bool ws = url.rfind("ws://", 0) == 0;
  if (!wss && !ws) return false;
  tls = wss;
  size_t hb = url.find("://") + 3;
  size_t slash = url.find('/', hb);
  std::string hostport = (slash == std::string::npos)
                             ? url.substr(hb)
                             : url.substr(hb, slash - hb);
  path = (slash == std::string::npos) ? "/" : url.substr(slash);
  size_t colon = hostport.find(':');
  if (colon == std::string::npos) {
    host = hostport;
    port = tls ? 443 : 80;
  } else {
    host = hostport.substr(0, colon);
    port = atoi(hostport.substr(colon + 1).c_str());
  }
  return !host.empty() && port > 0;
}

bool MiniWs::begin(const std::string& url, const std::string& extra_headers) {
  close();
  bool tls = true;
  std::string host, path;
  int port = 443;
  if (!parseUrl(url, tls, host, port, path)) {
    fail("bad url");
    return false;
  }
  delete cli_;
  cli_ = tls ? (Client*)new SecureCli() : (Client*)new PlainCli();
  if (!cli_->connect(host.c_str(), port)) {
    fail("tcp connect failed");
    return false;
  }
  if (!doHandshake(host, port, path, extra_headers, tls)) {
    cli_->stop();
    return false;
  }
  handshake_done_ = true;
  open_ = true;
  if (ev.on_open) ev.on_open();
  return true;
}

bool MiniWs::doHandshake(const std::string& host, int port,
                         const std::string& path,
                         const std::string& extra_headers, bool tls) {
  (void)port;
  (void)tls;
  std::string key = randKey();
  std::string req = "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n";
  if (!extra_headers.empty()) {
    req += extra_headers;
    if (req.size() < 2 || req.substr(req.size() - 2) != "\r\n") req += "\r\n";
  }
  req += "\r\n";
  if (cli_->write((const uint8_t*)req.data(), req.size()) != req.size()) {
    fail("handshake write failed");
    return false;
  }
  std::string status;
  if (!readHttpResponse(status)) {
    fail("handshake read failed");
    return false;
  }
  if (status.find("101") == std::string::npos) {
    fail("handshake not 101: " + status.substr(0, 64));
    return false;
  }
  std::string expect = wsAccept(key);
  if (status.find(expect.c_str()) == std::string::npos) {
    fail("accept mismatch");
    return false;
  }
  return true;
}

bool MiniWs::readHttpResponse(std::string& out) {
  out.clear();
  uint32_t t0 = millis();
  std::string acc;
  uint8_t b = 0;
  while (millis() - t0 < 10000) {
    if (!cli_->connected()) return false;
    if (cli_->available() <= 0) {
      delay(5);
      continue;
    }
    if (cli_->read(&b, 1) != 1) return false;
    acc += (char)b;
    if (acc.size() > 8192) return false;
    if (acc.size() >= 4 &&
        acc.substr(acc.size() - 4) == "\r\n\r\n") {
      out = acc;
      return true;
    }
  }
  return false;
}

bool MiniWs::readFrame(int& opcode, std::vector<uint8_t>& payload,
                      bool& fin) {
  payload.clear();
  fin = false;
  uint8_t hdr[2];
  if (cli_->available() < 2) return false;
  if (cli_->read(hdr, 2) != 2) return false;
  fin = (hdr[0] & 0x80) != 0;
  opcode = hdr[0] & 0x0f;
  bool masked = (hdr[1] & 0x80) != 0;
  uint64_t len = hdr[1] & 0x7f;
  if (len == 126) {
    uint8_t e[2];
    if (cli_->read(e, 2) != 2) return false;
    len = ((uint64_t)e[0] << 8) | e[1];
  } else if (len == 127) {
    uint8_t e[8];
    if (cli_->read(e, 8) != 8) return false;
    len = 0;
    for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (cli_->read(mask, 4) != 4) return false;
  }
  // TEXT は 8KB 上限 (字幕 JSON 用)、BINARY は 64KB まで受ける。
  // (下り Opus 生フレームは小さい想定。将来 v2/v3 対応時に再検討)
  const uint64_t kMax =
      (opcode == 0x2) ? 65536u : 8192u;
  if (len > kMax) {
    // 大きすぎる下り (音声バイナリ連続等) は読捨てて close 扱いにしない。
    // ここではフレームを破棄する。
    uint8_t tmp[256];
    uint64_t left = len;
    while (left > 0) {
      size_t n = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
      int r = cli_->read(tmp, n);
      if (r <= 0) break;
      if (masked) {
        // マスク解除は破棄なので省略可だが位置合わせのため不要
      }
      left -= r;
    }
    opcode = -1;
    return true;  // 捨てた
  }
  payload.resize((size_t)len);
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < len && millis() - t0 < 5000) {
    int av = cli_->available();
    if (av <= 0) {
      delay(1);
      continue;
    }
    uint8_t tmp[256];
    size_t want = len - got > sizeof(tmp) ? sizeof(tmp) : (size_t)(len - got);
    int r = cli_->read(tmp, want);
    if (r <= 0) return false;
    for (int i = 0; i < r; i++) {
      uint8_t v = tmp[i];
      if (masked) v ^= mask[(got + i) % 4];
      payload[got + i] = v;
    }
    got += r;
  }
  if (got != len) return false;
  return true;
}

bool MiniWs::writeFrame(int opcode, const uint8_t* data, size_t len) {
  if (!cli_ || !cli_->connected()) return false;
  uint8_t hdr[10];
  size_t hlen = 2;
  hdr[0] = 0x80 | (opcode & 0x0f);
  uint8_t mask[4];
  for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(esp_random() & 0xff);
  if (len < 126) {
    hdr[1] = 0x80 | (uint8_t)len;
  } else if (len < 65536) {
    hdr[1] = 0x80 | 126;
    hdr[2] = (len >> 8) & 0xff;
    hdr[3] = len & 0xff;
    hlen = 4;
  } else {
    return false;  // 送信は 64KB 未満のみ
  }
  if (cli_->write(hdr, hlen) != hlen) return false;
  if (cli_->write(mask, 4) != 4) return false;
  for (size_t i = 0; i < len; i++) {
    uint8_t v = data[i] ^ mask[i % 4];
    if (cli_->write(&v, 1) != 1) return false;
  }
  return true;
}

bool MiniWs::sendText(const std::string& text) {
  if (!open_) return false;
  return writeFrame(0x1, (const uint8_t*)text.data(), text.size());
}

void MiniWs::loop() {
  if (!cli_ || !handshake_done_) return;
  if (!cli_->connected()) {
    setClosed();
    return;
  }
  // 受信バッファにある分だけフレーム処理 (最大4フレーム/loop で音声 loop を塞がない)
  for (int i = 0; i < 4; i++) {
    if (cli_->available() < 2) break;
    int opcode = 0;
    bool fin = false;
    std::vector<uint8_t> payload;
    if (!readFrame(opcode, payload, fin)) break;
    if (opcode == -1) {
      frag_buf_.clear();
      frag_opcode_ = -1;
      continue;  // 大きすぎて捨てた
    }
    if (opcode == 0x0) {
      // 継続フレーム: 組立中がなければ破棄
      if (frag_opcode_ < 0) continue;
      frag_buf_.insert(frag_buf_.end(), payload.begin(), payload.end());
      if (frag_buf_.size() > 65536u) {
        frag_buf_.clear();
        frag_opcode_ = -1;
        continue;
      }
      if (!fin) continue;
      int done_op = frag_opcode_;
      frag_opcode_ = -1;
      deliver(done_op, frag_buf_);
      frag_buf_.clear();
      continue;
    }
    if (!fin && (opcode == 0x1 || opcode == 0x2)) {
      // 分割開始: 蓄積して継続待ち
      frag_opcode_ = opcode;
      frag_buf_ = payload;
      continue;
    }
    if (opcode == 0x1 || opcode == 0x2) {
      deliver(opcode, payload);
    } else if (opcode == 0x9) {
      writeFrame(0xA, payload.data(), payload.size());  // pong
    } else if (opcode == 0x8) {
      setClosed();
      return;
    }
  }
}

void MiniWs::deliver(int opcode, const std::vector<uint8_t>& payload) {
  if (opcode == 0x1) {
    if (ev.on_text)
      ev.on_text(std::string((const char*)payload.data(), payload.size()));
  } else if (opcode == 0x2) {
    if (ev.on_binary) ev.on_binary(payload.data(), payload.size());
  }
}

void MiniWs::close() {
  if (cli_) {
    if (open_) writeFrame(0x8, nullptr, 0);
    cli_->stop();
    delete cli_;
    cli_ = nullptr;
  }
  open_ = false;
  handshake_done_ = false;
  frag_buf_.clear();
  frag_opcode_ = -1;
}

void MiniWs::fail(const std::string& msg) {
  if (ev.on_error) ev.on_error(msg);
}

void MiniWs::setClosed() {
  if (open_) {
    open_ = false;
    handshake_done_ = false;
    if (ev.on_close) ev.on_close();
  }
  if (cli_) {
    cli_->stop();
    delete cli_;
    cli_ = nullptr;
  }
}
