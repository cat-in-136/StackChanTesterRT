#pragma once
// core/ デバイス非依存型定義。Arduino / M5 / WiFi / Preferences を含めない。
// C++17 + STL のみ。
#include <cstdint>
#include <string>

namespace xiaozhi {

// min-ws-core-plan §2 依存ルール: core -> ports/Arduino/M5 include 禁止。
struct DeviceIdentity {
  std::string device_id;  // WiFi.macAddress() 由来 (ports 側で生成)
  std::string client_id;  // Preferences UUID 由来 (ports 側で生成)
};

struct ServerConfig {
  std::string url;    // wss://.../xiaozhi/v1/
  std::string token;  // Bearer token
};

struct ActivationInfo {
  bool present = false;  // activation ブロック有無
  std::string message;
  std::string code;       // 6桁
  std::string challenge;  // HMAC 用 (簡易系では空でも可)
  int timeout_ms = 0;
};

enum class TextFormat : uint8_t {
  A_DetectText = 0,  // {"type":"listen","state":"detect","text":...} 既定・互換優先
  B_TypeText = 1,    // {"type":"text","text":...} サーバ拡張用
};

enum class PipelineState : uint8_t {
  Idle = 0,
  Connecting,
  Listening,
  Speaking,
};

inline const char* toStr(PipelineState s) {
  switch (s) {
    case PipelineState::Idle:
      return "Idle";
    case PipelineState::Connecting:
      return "Connecting";
    case PipelineState::Listening:
      return "Listening";
    case PipelineState::Speaking:
      return "Speaking";
  }
  return "?";
}

}  // namespace xiaozhi
