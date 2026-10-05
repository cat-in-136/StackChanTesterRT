#pragma once
// Activation: CheckVersion / Activate の純粋ロジック (plan §3.2)
// HTTP 実行自体は ports 側。ここでは body 組立と応答 parse のみ。
#include <string>

#include "json_min.h"
#include "types.h"

namespace xiaozhi {

inline std::string activateUrlFor(const std::string& ota_url) {
  if (ota_url.empty()) return "activate";
  if (ota_url.back() == '/') return ota_url + "activate";
  return ota_url + "/activate";
}

// 最小 board JSON。公式は項目追加に寛容なため必須級のみ送る。
inline std::string buildCheckVersionBody(const DeviceIdentity& id) {
  std::string o = "{";
  o += "\"version\":2,";
  o += "\"language\":\"zh-CN\",";
  o += "\"mac_address\":\"" + json_min::escape(id.device_id) + "\",";
  o += "\"uuid\":\"" + json_min::escape(id.client_id) + "\",";
  o += "\"chip_model_name\":\"ESP32-S3\",";
  o += "\"board\":{\"type\":\"m5stack-cores3\",\"name\":\"m5stack-cores3\"},";
  o += "\"application\":{\"name\":\"StackChanRT-chatbot\",\"version\":\"0.1.0\"}";
  o += "}";
  return o;
}

// serial 無し簡易系は {} (ota.cc:459-496 互換の縮退)。HMAC は後回し。
inline std::string buildActivateBody(const std::string& /*challenge*/) {
  return "{}";
}

struct CheckVersionResult {
  bool ok = false;  // websocket 取得 or activation コード表示のいずれか成立
  ServerConfig server;
  MqttConfig mqtt;
  ActivationInfo activation;
  bool has_websocket = false;
};

// body から websocket{url,token} / mqtt{...} / activation{...} を抽出。
// 未バインド時 (activation のみで websocket 無し) も ok とする。
// firmware.url 等の誤拾い防止のため "websocket" オブジェクト内に限定する。
inline bool parseCheckVersionBody(const std::string& body,
                                  CheckVersionResult& out) {
  out = CheckVersionResult{};
  size_t ws_pos = body.find("\"websocket\"");
  if (ws_pos != std::string::npos) {
    // "websocket" の後の { ... } スコープを抜き出す (バランス考慮)。
    size_t brace = body.find('{', ws_pos);
    if (brace != std::string::npos) {
      int depth = 0;
      size_t end = std::string::npos;
      for (size_t i = brace; i < body.size(); i++) {
        if (body[i] == '{') depth++;
        if (body[i] == '}') {
          depth--;
          if (depth == 0) {
            end = i;
            break;
          }
        }
      }
      if (end != std::string::npos) {
        std::string scope = body.substr(brace, end - brace + 1);
        std::string url, token;
        if (json_min::extractString(scope, "url", url) &&
            json_min::extractString(scope, "token", token)) {
          out.server.url = url;
          out.server.token = token;
          out.has_websocket = !url.empty();
        }
      }
    }
  }
  {
    std::string scope;
    if (json_min::jsonScope(body, "mqtt", scope)) {
      out.mqtt.present = true;
      json_min::extractString(scope, "endpoint", out.mqtt.endpoint);
      json_min::extractString(scope, "client_id", out.mqtt.client_id);
      json_min::extractString(scope, "username", out.mqtt.username);
      json_min::extractString(scope, "password", out.mqtt.password);
      json_min::extractString(scope, "publish_topic", out.mqtt.publish_topic);
      json_min::extractString(scope, "subscribe_topic",
                              out.mqtt.subscribe_topic);
      if (out.mqtt.publish_topic.empty()) out.mqtt.publish_topic = "device-server";
    }
  }
  if (body.find("\"activation\"") != std::string::npos) {
    ActivationInfo a;
    a.present = true;
    json_min::extractString(body, "message", a.message);
    json_min::extractString(body, "code", a.code);
    json_min::extractString(body, "challenge", a.challenge);
    int t = 0;
    if (json_min::extractInt(body, "timeout_ms", t)) a.timeout_ms = t;
    out.activation = a;
  }
  out.ok = out.has_websocket || out.mqtt.present ||
           (out.activation.present && !out.activation.code.empty());
  return out.ok;
}

}  // namespace xiaozhi
