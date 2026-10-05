#pragma once
// ports: MiniWs ラッパ (plan ArduinoWsTransport 相当)
// wss:// のみ。ヘッダ4種を付与。受信 TEXT->on_text, BIN->on_binary へ転送。
#include <stdint.h>

#include <functional>
#include <string>

#include "mini_ws.h"

class ArduinoWs {
 public:
  struct Events {
    std::function<void()> on_open;
    std::function<void(const std::string& text)> on_text;
    std::function<void(const uint8_t* data, size_t len)> on_binary;
    std::function<void()> on_close;
  };
  Events ev;

  ArduinoWs();
  // wss://host[:port]/path を MiniWs に委譲。header 4種を付与。
  bool begin(const std::string& wss_url, const std::string& token,
             const std::string& device_id, const std::string& client_id);
  void loop();
  bool sendText(const std::string& text);
  void close();
  bool isConnected() const { return ws_.isOpen(); }

 private:
  MiniWs ws_;
};
