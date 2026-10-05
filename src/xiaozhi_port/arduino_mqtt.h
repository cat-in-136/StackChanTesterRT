#pragma once
// ports: PubSubClient (knolleary) ラッパによる MQTT 3.1.1 制御チャネル。
// TLS (WiFiClientSecure + CA bundle) / keepalive / QoS1 subscribe 対応。
// 受信 PUBLISH の payload を on_text へ転送。音声 (UDP AES-CTR) は後段実装。
#include <functional>
#include <string>

#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include "../xiaozhi_core/types.h"

class ArduinoMqtt {
 public:
  struct Events {
    std::function<void()> on_open;
    std::function<void(const std::string& text)> on_text;
    std::function<void(const std::string& msg)> on_error;
    std::function<void()> on_close;
  };
  Events ev;

  ArduinoMqtt();
  // 同期接続 (TCP+TLS+CONNECT+SUBSCRIBE)。成功で ev.on_open を fire。
  // 戻り値は接続・購読成功可否。失敗時は isConnected()=false でリトライ待ち。
  bool begin(const xiaozhi::MqttConfig& cfg,
             const std::string& subscribe_topic);
  void loop();  // PubSubClient::loop (受信処理 + keepalive ping)
  bool sendText(const std::string& text);  // publish_topic へ QoS0 publish
  void close();
  bool isConnected() { return mqtt_.connected(); }

 private:
  void onMessage(const std::string& topic, const uint8_t* payload,
                 size_t len);

  WiFiClientSecure tls_;
  PubSubClient mqtt_;
  std::string publish_topic_;
};