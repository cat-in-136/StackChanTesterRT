#include "mqtt.h"

#include "network.h"

#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// PubSubClient ラッパ。制御チャネル (hello / mcp / listen / tts) 用。
// subscribe_topic_ は begin() に渡された完成値を使用 (chatbot 側で派生)。

Mqtt::Mqtt() { mqtt_.setClient(tls_); }

void Mqtt::onMessage(const std::string& /*topic*/,
                            const uint8_t* payload, size_t len) {
  std::string text((const char*)payload, len);
  if (ev.on_text) ev.on_text(text);
}

bool Mqtt::begin(const xiaozhi::MqttConfig& cfg,
                        const std::string& subscribe_topic) {
  if (cfg.endpoint.empty()) return false;
  std::string host = cfg.endpoint;
  uint16_t port = 8883;
  size_t colon = cfg.endpoint.find(':');
  if (colon != std::string::npos) {
    host = cfg.endpoint.substr(0, colon);
    int p = atoi(cfg.endpoint.substr(colon + 1).c_str());
    if (p > 0) port = (uint16_t)p;
  }
  publish_topic_ =
      cfg.publish_topic.empty() ? "device-server" : cfg.publish_topic;

  mqtt_.setServer(host.c_str(), port);
  mqtt_.setKeepAlive(240);
  mqtt_.setBufferSize(1024);
  mqtt_.setCallback([this](char* topic, uint8_t* payload, unsigned int len) {
    onMessage(topic, payload, len);
  });

  if (tls_.connected()) tls_.stop();
  applyTlsBundle(tls_);
  mqtt_.setClient(tls_);

  bool rc = mqtt_.connect(cfg.client_id.c_str(), cfg.username.c_str(),
                          cfg.password.c_str());
  if (!rc) {
    if (ev.on_error)
      ev.on_error("mqtt connect rc=" + std::to_string(mqtt_.state()));
    return false;
  }
  if (!mqtt_.subscribe(subscribe_topic.c_str(), 1)) {
    if (ev.on_error) ev.on_error("mqtt subscribe failed");
    mqtt_.disconnect();
    return false;
  }
  tls_.flush();
  if (ev.on_open) ev.on_open();
  return true;
}

void Mqtt::loop() {
  if (!mqtt_.connected()) return;  // 切断済み: 再接続は chatbot 側リトライ
  mqtt_.loop();
  if (!mqtt_.connected()) {
    if (ev.on_close) ev.on_close();
  }
}

bool Mqtt::sendText(const std::string& text) {
  if (!mqtt_.connected()) return false;
  bool ok = mqtt_.publish(publish_topic_.c_str(), text.c_str(), false);
  // mbedTLS の送信バッファは次の read までフラッシュされない場合がある。
  // publish 後はサーバが応答を送るまで read が空のため、明示 flush で
  // 送信を完了させる (CONNECT/SUBSCRIBE は直後の read でフラッシュされる)。
  tls_.flush();
  return ok;
}

void Mqtt::close() {
  mqtt_.disconnect();
  tls_.stop();
}