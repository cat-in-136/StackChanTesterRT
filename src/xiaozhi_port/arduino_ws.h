#pragma once
// ports: WebSocketsClient ラッパ (plan ArduinoWsTransport 相当)
// wss:// (TLS bundle検証) / ws:// 対応。ヘッダ4種を付与。受信 TEXT->on_text, BIN->on_binary へ転送。
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include <WebSocketsClient.h>

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
  // wss://host[:port]/path を WebSocketsClient に委譲。header 4種を付与。
  // 非同期接続のため戻り値は URL parse可否のみ。接続完了は on_open / isConnected() で判断。
  bool begin(const std::string& wss_url, const std::string& token,
             const std::string& device_id, const std::string& client_id);
  void loop();
  bool sendText(const std::string& text);
  void close();
  bool isConnected() { return ws_.isConnected(); }

 private:
  void handleEvent(WStype_t type, uint8_t* payload, size_t len);
  static bool parseUrl(const std::string& url, bool& tls, std::string& host,
                       int& port, std::string& path);

  WebSocketsClient ws_;
  // setExtraHeaders() に渡す文字列の寿命保持用
  std::string extraHeaders_;
  // FRAGMENT_* 再組み立て用 (ライブラリはアプリ側組み立て要求)
  std::vector<uint8_t> fragBuf_;
  bool fragInProgress_ = false;
  bool fragIsText_ = true;
};
