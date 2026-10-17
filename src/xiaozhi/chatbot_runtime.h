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
#include "arduino/mic_capture.h"
#include "arduino/mqtt.h"
#include "arduino/network.h"
#include "arduino/opus_encoder.h"
#include "arduino/opus_player.h"
#include "arduino/udp_audio.h"

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
  // 音声トグル (ToggleChatState 相当: Idle→開始 / Listening→停止 /
  // Speaking→Abort)。未接続時は start() (再接続) にフォールバック。
  void toggleVoice();
  bool voiceActive() const { return voice_active_; }
  void setFormatB(bool use_b);
  bool getFormatB() const;
  void setVolume(uint8_t volume);
  uint8_t getVolume() const { return volume_; }
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
  // UDP音声下りの開始 (hello応答の udp{} 使用)。NAT punch 含む。
  void openUdpAudio();
  void closeUdpAudio();
  // NAT維持用: 最終送信から10s以上なら無音フレームを1発送る。
  void punchIfStale(uint32_t now, bool force = false);
  // 上りポンプ: Listening+voice_active_ の間のみ mic→encode→UDP送信。
  // 半二重のため Speaking 中は呼ばない (tts_start で mic 停止済み)。
  void pumpVoice(uint32_t now);
  // listen/start(auto)+mic開始。false=未接続/mic失敗 (送信なし)。
  bool startVoice();
  // listen/stop 送信 + Idle (ToggleChatState の Listening→停止)。
  void stopVoice();
  // abort 送信 + 再生即時停止 + Idle (同 Speaking→Abort)。接続は維持。
  void abortVoice();
  // Speaking中の口パク駆動 (180msトグル)。loop から呼ぶ。
  void driveMouth(uint32_t now);
  void closeMouth();
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
  UdpAudio udp_;
  OpusPlayer player_;
  MicCapture mic_;
  OpusVoiceEncoder venc_;
  TextFormat fmt_ = TextFormat::A_DetectText;
  // chatbot 音声出力 (M5.Speaker ch1) の音量 (0-255)。
  // 128: master 128 との組み合わせで original (16×255) の約4倍。
  uint8_t volume_ = 128;
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
  // 音声セッション中 (listen/start〜stop/abort)。Touch to Talk (manual)
  // の一発話単位。stop/abort/stopVoice・切断で終了する。
  bool voice_active_ = false;
  uint32_t voice_tx_count_ = 0;
  uint32_t voice_tx_logged_ = 0;
  // 上り1フレーム作業域 (60ms PCM + Opus)。
  int16_t voice_pcm_[MicCapture::kFrameSamples];
  uint8_t voice_opus_[OpusVoiceEncoder::kMaxPacket];

  bool time_synced_ = false;

  uint32_t last_punch_ms_ = 0;
  uint32_t mouth_last_ms_ = 0;
  bool mouth_open_ = false;

  // status()/lastChat() 用の静的バッファ。
  char status_buf_[160];
  char chat_buf_[192];
};

}  // namespace xiaozhi