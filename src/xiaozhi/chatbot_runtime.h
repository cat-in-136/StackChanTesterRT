#pragma once
// ChatbotRuntime: chatbot.cpp の内部から切り出した実行時進行管理。
// 起動 (Wi-Fi/OTA/activation)・MQTT 接続・hello・状態遷移・再試行を担う。
// 公開 API (chatbot.h) の実装は facade (chatbot.cpp) 側が行う。
// アプリ固有の M5/Avatar 表示は callback 経由で行い、runtime 自体は保持しない。
#include <ArduinoJson.h>
#include <WiFi.h>

#include <cstdint>
#include <string>

#include "core/activation.h"
#include "core/debug_text_channel.h"
#include "core/mqtt_session.h"
#include "core/voice_pipeline.h"
#include "arduino/auth_store.h"
#include "arduino/mqtt.h"
#include "arduino/network.h"

namespace xiaozhi {

class ChatbotRuntime {
 public:
  void setup();
  void loop();
  void teardown();

  bool sendText(const char* text);
  void start();
  void stop();
  void abort();
  void setFormatB(bool use_b);
  bool getFormatB() const;
  std::string status() const;
  std::string lastChat() const;
  bool isActive() const { return active_; }

 private:
  enum Phase : uint8_t {
    PhIdle = 0,
    PhWifi,
    PhOta,
    PhActivationWait,
    PhMqttConnect,
    PhHelloWait,
    PhReady,
  };

  static const char* phaseStr(Phase p);
  void setPhase(Phase p);

  void showText(const char* text);
  void applyEmotion(const std::string& e);

  bool ensureWifi();
  void startWifi();
  bool ensureTimeSync();
  void doOtaOnce();
  bool doActivateOnce();
  void connectMqtt();
  void sendHello();
  void setupPipeCallbacks();
  void setupMqttCallbacks();
  bool tryHandleMcp(const std::string& text);

  bool hasRuntimeServer() const;
  bool transportConnected();
  void transportLoop();
  bool transportSend(const std::string& text);
  void transportClose();

  AuthStore auth_;
  Mqtt mqtt_;
  VoicePipeline pipe_;
  TextFormat fmt_ = TextFormat::A_DetectText;
  ServerHelloMqtt hello_mqtt_;

  bool active_ = false;
  Phase phase_ = PhIdle;
  uint32_t phase_since_ = 0;
  uint32_t hello_sent_ms_ = 0;
  uint32_t last_retry_ms_ = 0;
  int hello_fail_count_ = 0;
  int ota_attempts_ = 0;
  bool need_activation_ = false;
  std::string activation_code_;
  uint32_t last_activate_ms_ = 0;
  int activate_attempts_ = 0;

  MqttConfig mqtt_cfg_;
  std::string mqtt_sub_topic_;

  std::string last_user_;
  std::string last_assistant_;
  std::string last_emotion_;
  std::string last_error_;
  bool listen_started_ = false;

  bool time_synced_ = false;

  // status()/lastChat() 用の静的バッファ。
  char status_buf_[160];
  char chat_buf_[192];
};

}  // namespace xiaozhi