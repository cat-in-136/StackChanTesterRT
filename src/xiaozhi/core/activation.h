#pragma once
// Activation: CheckVersion / Activate の純粋ロジック (plan §3.2)
// HTTP 実行自体は ports 側。ここでは body 組立と応答 parse のみ。
#include <ArduinoJson.h>

#include <string>

#include "types.h"

namespace xiaozhi {

inline std::string activateUrlFor(const std::string& ota_url) {
  if (ota_url.empty()) return "activate";
  if (ota_url.back() == '/') return ota_url + "activate";
  return ota_url + "/activate";
}

// 最小 board JSON。公式は項目追加に寛容なため必須級のみ送る。
inline std::string buildCheckVersionBody(const DeviceIdentity& id) {
  JsonDocument doc;
  doc["version"] = 2;
  doc["language"] = "zh-CN";
  doc["mac_address"] = id.device_id;
  doc["uuid"] = id.client_id;
  doc["chip_model_name"] = "ESP32-S3";
  doc["board"]["type"] = "m5stack-cores3";
  doc["board"]["name"] = "m5stack-cores3";
  doc["application"]["name"] = "StackChanRT-chatbot";
  doc["application"]["version"] = "0.1.0";
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

// serial 無し簡易系は {} (ota.cc:459-496 互換の縮退)。HMAC は後回し。
inline std::string buildActivateBody(const std::string& /*challenge*/) {
  return "{}";
}

struct CheckVersionResult {
  bool ok = false;  // mqtt 取得 or activation コード表示のいずれか成立
  MqttConfig mqtt;
  ActivationInfo activation;
};

// body から mqtt{...} / activation{...} を抽出。
inline bool parseCheckVersionBody(const std::string& body,
                                  CheckVersionResult& out) {
  out = CheckVersionResult{};
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str())) return false;
  JsonVariant mq = doc["mqtt"];
  if (!mq.isNull()) {
    out.mqtt.present = true;
    const char* endpoint = mq["endpoint"];
    const char* client_id = mq["client_id"];
    const char* username = mq["username"];
    const char* password = mq["password"];
    const char* pub = mq["publish_topic"];
    const char* sub = mq["subscribe_topic"];
    if (endpoint) out.mqtt.endpoint = endpoint;
    if (client_id) out.mqtt.client_id = client_id;
    if (username) out.mqtt.username = username;
    if (password) out.mqtt.password = password;
    if (pub) out.mqtt.publish_topic = pub;
    if (sub) out.mqtt.subscribe_topic = sub;
    if (out.mqtt.publish_topic.empty()) out.mqtt.publish_topic = "device-server";
  }
  JsonVariant ac = doc["activation"];
  if (!ac.isNull()) {
    ActivationInfo a;
    a.present = true;
    const char* message = ac["message"];
    const char* code = ac["code"];
    const char* challenge = ac["challenge"];
    if (message) a.message = message;
    if (code) a.code = code;
    if (challenge) a.challenge = challenge;
    if (!ac["timeout_ms"].isNull()) a.timeout_ms = ac["timeout_ms"].as<int>();
    out.activation = a;
  }
  out.ok = out.mqtt.present ||
           (out.activation.present && !out.activation.code.empty());
  return out.ok;
}

}  // namespace xiaozhi
