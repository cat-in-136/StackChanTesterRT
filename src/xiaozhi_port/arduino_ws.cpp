#include "arduino_ws.h"

#include "tls_bundle.h"

// embed_files (x509_crt_bundle) の終端シンボル。size 付き API 用。
extern const uint8_t tls_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

ArduinoWs::ArduinoWs() {
  ws_.onEvent([this](WStype_t type, uint8_t* payload, size_t len) {
    handleEvent(type, payload, len);
  });
  // 切断時の自動再接続はライブラリに任せず chatbot 側の 5s リトライに一本化。
  // ライブラリ既定(短間隔)での裏再接続を抑止するため長めに設定。
  ws_.setReconnectInterval(10000);
}

bool ArduinoWs::parseUrl(const std::string& url, bool& tls, std::string& host,
                         int& port, std::string& path) {
  bool wss = url.rfind("wss://", 0) == 0;
  bool ws = url.rfind("ws://", 0) == 0;
  if (!wss && !ws) return false;
  tls = wss;
  size_t hb = url.find("://") + 3;
  size_t slash = url.find('/', hb);
  std::string hostport = (slash == std::string::npos) ? url.substr(hb)
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

bool ArduinoWs::begin(const std::string& wss_url, const std::string& token,
                      const std::string& device_id,
                      const std::string& client_id) {
  bool tls = true;
  std::string host, path;
  int port = 443;
  if (!parseUrl(wss_url, tls, host, port, path)) return false;

  // 公式は token 空なら Authorization を省略 (websocket_protocol.cc:97-103)
  extraHeaders_.clear();
  if (!token.empty()) extraHeaders_ += "Authorization: Bearer " + token + "\r\n";
  extraHeaders_ += "Protocol-Version: 1\r\n";
  extraHeaders_ += "Device-Id: " + device_id + "\r\n";
  extraHeaders_ += "Client-Id: " + client_id + "\r\n";
  ws_.setExtraHeaders(extraHeaders_.c_str());

  fragBuf_.clear();
  fragInProgress_ = false;

  // protocol 空 = Sec-WebSocket-Protocol を送らない (従来互換)
  if (tls) {
#ifdef CHATBOT_INSECURE_FALLBACK
    ws_.beginSSL(host.c_str(), (uint16_t)port, path.c_str(), (const uint8_t*)NULL,
                 "");
#else
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 4)
    ws_.beginSslWithBundle(host.c_str(), (uint16_t)port, path.c_str(),
                           tls_crt_bundle_start,
                           (size_t)(tls_crt_bundle_end - tls_crt_bundle_start),
                           "");
#else
    ws_.beginSslWithBundle(host.c_str(), (uint16_t)port, path.c_str(),
                           tls_crt_bundle_start, "");
#endif
#endif
  } else {
    ws_.begin(host.c_str(), (uint16_t)port, path.c_str(), "");
  }
  return true;
}

void ArduinoWs::handleEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      fragBuf_.clear();
      fragInProgress_ = false;
      if (ev.on_open) ev.on_open();
      break;
    case WStype_TEXT:
      if (ev.on_text) {
        ev.on_text(std::string(payload ? (const char*)payload : "", len));
      }
      break;
    case WStype_BIN:
      if (ev.on_binary) ev.on_binary(payload, len);
      break;
    case WStype_FRAGMENT_TEXT_START:
      fragBuf_.assign(payload, payload + len);
      fragInProgress_ = true;
      fragIsText_ = true;
      break;
    case WStype_FRAGMENT_BIN_START:
      fragBuf_.assign(payload, payload + len);
      fragInProgress_ = true;
      fragIsText_ = false;
      break;
    case WStype_FRAGMENT:
      if (fragInProgress_) fragBuf_.insert(fragBuf_.end(), payload, payload + len);
      break;
    case WStype_FRAGMENT_FIN:
      if (fragInProgress_) {
        fragBuf_.insert(fragBuf_.end(), payload, payload + len);
        if (fragIsText_) {
          if (ev.on_text) {
            ev.on_text(std::string((const char*)fragBuf_.data(), fragBuf_.size()));
          }
        } else {
          if (ev.on_binary) ev.on_binary(fragBuf_.data(), fragBuf_.size());
        }
        fragBuf_.clear();
        fragInProgress_ = false;
      }
      break;
    case WStype_DISCONNECTED:
      fragBuf_.clear();
      fragInProgress_ = false;
      if (ev.on_close) ev.on_close();
      break;
    case WStype_ERROR:
      if (ev.on_error) {
        ev.on_error(std::string(payload ? (const char*)payload : "", len));
      }
      break;
    case WStype_PING:
    case WStype_PONG:
    default:
      break;
  }
}

void ArduinoWs::loop() { ws_.loop(); }

bool ArduinoWs::sendText(const std::string& text) {
  if (!ws_.isConnected()) return false;
  return ws_.sendTXT(text.c_str(), text.size());
}

void ArduinoWs::close() {
  fragBuf_.clear();
  fragInProgress_ = false;
  ws_.disconnect();
}
