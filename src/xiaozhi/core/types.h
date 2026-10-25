#pragma once
// core/ デバイス非依存型定義。画面表示など通信外処理の混入を避けるための分離であり、
// Arduino / M5Unified およびメジャーなライブラリ (ArduinoJson 等) の使用自体は許容する。
#include <cstdint>
#include <string>

namespace xiaozhi {

// core は通信ロジックに専念し、画面・入出力デバイス依存を持ち込まない (ports 側に委譲)。
struct DeviceIdentity {
  std::string device_id;  // WiFi.macAddress() 由来 (ports 側で生成)
  std::string client_id;  // Preferences UUID 由来 (ports 側で生成)
};

// OTA 応答の mqtt ブロック。
// subscribe_topic が "null"/空の場合は device_id から派生する
// (devices/p2p/{mac_underscore_lower})。
struct MqttConfig {
  bool present = false;
  std::string endpoint;        // ホスト[:ポート]
  std::string client_id;
  std::string username;
  std::string password;
  std::string publish_topic;   // "device-server"
  std::string subscribe_topic;
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
  Idle = 0,      // 待機 (セッション維持・未録音)
  Connecting,    // (initial) hello 待ち
  Listening,     // 録音中 (mic 上り有効)
  Processing,    // 処理待ち (listen/stop 送信後〜tts/start 前)
  Speaking,      // 発話(再生)
};

inline const char* toStr(PipelineState s) {
  switch (s) {
    case PipelineState::Idle:
      return "Idle";
    case PipelineState::Connecting:
      return "Connecting";
    case PipelineState::Listening:
      return "Listening";
    case PipelineState::Processing:
      return "Processing";
    case PipelineState::Speaking:
      return "Speaking";
  }
  return "?";
}

}  // namespace xiaozhi
