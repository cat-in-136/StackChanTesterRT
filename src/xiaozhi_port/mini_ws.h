#pragma once
// 最小 WSS クライアント (外部 links2004/WebSockets 不使用)。
// テキスト PoC 用: TEXT 送受信 + BINARY 受信 + ping 自動 pong + close 検出。
// WiFiClientSecure (wss) / WiFiClient (ws) 上の RFC6455 最小実装。
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

class MiniWs {
 public:
  class Client {
   public:
    virtual ~Client() {}
    virtual bool connect(const char* host, int port) = 0;
    virtual void stop() = 0;
    virtual bool connected() = 0;
    virtual int available() = 0;
    virtual int read(uint8_t* buf, size_t n) = 0;
    virtual size_t write(const uint8_t* buf, size_t n) = 0;
    virtual void setTimeout(int ms) = 0;
  };
  struct Events {
    std::function<void()> on_open;  // ハンドシェイク完了
    std::function<void(const std::string& text)> on_text;
    std::function<void(const uint8_t* data, size_t len)> on_binary;
    std::function<void()> on_close;
    std::function<void(const std::string& msg)> on_error;
  };
  Events ev;

  MiniWs();
  ~MiniWs();

  // wss://host[:port]/path 形式。extra_headers は "K: V\r\n..." 追加分。
  bool begin(const std::string& url, const std::string& extra_headers);
  void loop();  // 受信ポンプ。chatbot loop から毎回呼ぶ。
  bool sendText(const std::string& text);
  void close();
  bool isOpen() const { return open_; }

 private:
  bool parseUrl(const std::string& url, bool& tls, std::string& host,
                int& port, std::string& path);
  bool doHandshake(const std::string& host, int port, const std::string& path,
                   const std::string& extra_headers, bool tls);
  bool readHttpResponse(std::string& out_status);
  // fin は受信フレームの FIN ビット。opcode 0x0=継続。
  bool readFrame(int& opcode, std::vector<uint8_t>& payload, bool& fin);
  bool writeFrame(int opcode, const uint8_t* data, size_t len);
  void fail(const std::string& msg);
  void setClosed();
  void deliver(int opcode, const std::vector<uint8_t>& payload);

  Client* cli_ = nullptr;
  bool open_ = false;
  bool handshake_done_ = false;
  std::vector<uint8_t> rx_;
  // fragment 再組み立て用 (公式下位スタック対応相当)
  std::vector<uint8_t> frag_buf_;
  int frag_opcode_ = -1;  // -1=組立中なし、0x1=text、0x2=binary
};
