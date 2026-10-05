#include "arduino_ws.h"

ArduinoWs::ArduinoWs() {
  ws_.ev.on_open = [this]() {
    if (ev.on_open) ev.on_open();
  };
  ws_.ev.on_text = [this](const std::string& t) {
    if (ev.on_text) ev.on_text(t);
  };
  ws_.ev.on_binary = [this](const uint8_t* d, size_t n) {
    if (ev.on_binary) ev.on_binary(d, n);
  };
  ws_.ev.on_close = [this]() {
    if (ev.on_close) ev.on_close();
  };
  ws_.ev.on_error = [](const std::string&) {};
}

bool ArduinoWs::begin(const std::string& wss_url, const std::string& token,
                      const std::string& device_id,
                      const std::string& client_id) {
  // 公式は token 空なら Authorization を省略 (websocket_protocol.cc:97-103)
  std::string headers;
  if (!token.empty()) headers += "Authorization: Bearer " + token + "\r\n";
  headers += "Protocol-Version: 1\r\n";
  headers += "Device-Id: " + device_id + "\r\n";
  headers += "Client-Id: " + client_id + "\r\n";
  return ws_.begin(wss_url, headers);
}

void ArduinoWs::loop() { ws_.loop(); }

bool ArduinoWs::sendText(const std::string& text) {
  return ws_.sendText(text);
}

void ArduinoWs::close() { ws_.close(); }
