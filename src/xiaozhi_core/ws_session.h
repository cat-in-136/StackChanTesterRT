#pragma once
// WsSession: hello 組立・server hello 解析・10s待ち定数 (plan §3.1)
#include <ArduinoJson.h>

#include <cstring>
#include <string>

#include "types.h"

namespace xiaozhi {

// public server 固定。設定不要 (OTA URL の切替はビルド時定数で)。
static const char DEFAULT_OTA_URL[] = "https://api.tenclass.net/xiaozhi/ota/";
static const int kHelloTimeoutMs = 10000;

// D->S hello (v1 固定)。公式は features.mcp:true が必須級。
inline std::string buildHello() {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["version"] = 1;
  doc["features"]["mcp"] = true;
  doc["transport"] = "websocket";
  doc["audio_params"]["format"] = "opus";
  doc["audio_params"]["sample_rate"] = 16000;
  doc["audio_params"]["channels"] = 1;
  doc["audio_params"]["frame_duration"] = 60;
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

// D->S hello (MQTT 制御チャネル用: transport=udp, version=3)。
// 公式 mqtt_protocol.cc GetHelloMessage 互換。UDP は後段で実装予定。
inline std::string buildMqttHello() {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["version"] = 3;
  doc["features"]["mcp"] = true;
  doc["transport"] = "udp";
  doc["audio_params"]["format"] = "opus";
  doc["audio_params"]["sample_rate"] = 16000;
  doc["audio_params"]["channels"] = 1;
  doc["audio_params"]["frame_duration"] = 60;
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

struct ServerHello {
  std::string session_id;  // 公式は任意。空でも受理する。
  int sample_rate = 24000;  // 下り既定 (protocol.h:79)
  int frame_duration = 60;
};

// type==hello && transport==websocket のみ受理。session_id 欠落でも true。
// (公式 websocket_protocol.cc:236-254 は sid 無しでも event set する)
inline bool parseServerHello(const std::string& text, ServerHello& out) {
  JsonDocument doc;
  if (deserializeJson(doc, text.c_str())) return false;
  const char* type = doc["type"];
  if (!type || std::strcmp(type, "hello") != 0) return false;
  const char* transport = doc["transport"];
  if (!transport || std::strcmp(transport, "websocket") != 0) return false;
  // session_id は任意 (公式は保存のみ)。欠落・空でも受理する。
  const char* sid = doc["session_id"];
  out.session_id = sid ? sid : "";
  if (!doc["sample_rate"].isNull()) {
    int sr = doc["sample_rate"].as<int>();
    if (sr > 0) out.sample_rate = sr;
  }
  if (!doc["frame_duration"].isNull()) {
    int fd = doc["frame_duration"].as<int>();
    if (fd > 0) out.frame_duration = fd;
  }
  return true;
}

// ---- MQTT 制御チャネル (transport=udp) 用 ----

struct ServerHelloMqtt {
  bool ok = false;
  std::string session_id;
  int sample_rate = 24000;   // 下り既定 (protocol.h:79)
  int frame_duration = 60;
  std::string udp_server;    // UDP 音声は後段実装。現 PoC はログのみ。
  int udp_port = 0;
  std::string udp_key;       // hex (AES-128)
  std::string udp_nonce;     // hex
};

// type==hello && transport==udp のみ受理。udp{server,port,key,nonce} を抽出。
inline ServerHelloMqtt parseServerHelloMqtt(const std::string& text) {
  ServerHelloMqtt h;
  JsonDocument doc;
  if (deserializeJson(doc, text.c_str())) return h;
  const char* type = doc["type"];
  if (!type || std::strcmp(type, "hello") != 0) return h;
  const char* transport = doc["transport"];
  if (!transport || std::strcmp(transport, "udp") != 0) return h;
  h.ok = true;
  const char* sid = doc["session_id"];
  h.session_id = sid ? sid : "";
  if (!doc["sample_rate"].isNull()) {
    int sr = doc["sample_rate"].as<int>();
    if (sr > 0) h.sample_rate = sr;
  }
  if (!doc["frame_duration"].isNull()) {
    int fd = doc["frame_duration"].as<int>();
    if (fd > 0) h.frame_duration = fd;
  }
  JsonVariant udp = doc["udp"];
  if (!udp.isNull()) {
    const char* server = udp["server"];
    if (server) h.udp_server = server;
    if (!udp["port"].isNull()) h.udp_port = udp["port"].as<int>();
    const char* key = udp["key"];
    if (key) h.udp_key = key;
    const char* nonce = udp["nonce"];
    if (nonce) h.udp_nonce = nonce;
  }
  return h;
}

// MCP initialize への応答 (公式 mcp_server.cc:417-430 互換)。
inline std::string buildMcpInitializeResult(const std::string& session_id,
                                            int id) {
  JsonDocument doc;
  doc["session_id"] = session_id;
  doc["type"] = "mcp";
  doc["payload"]["jsonrpc"] = "2.0";
  doc["payload"]["id"] = id;
  doc["payload"]["result"]["protocolVersion"] = "2024-11-05";
  doc["payload"]["result"]["capabilities"]["tools"] = JsonObject();
  doc["payload"]["result"]["serverInfo"]["name"] = "m5stack-cores3";
  doc["payload"]["result"]["serverInfo"]["version"] = "0.1.0";
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

// MCP tools/list への応答 (空ツール一覧)。
inline std::string buildMcpToolsListResult(const std::string& session_id,
                                           int id) {
  JsonDocument doc;
  doc["session_id"] = session_id;
  doc["type"] = "mcp";
  doc["payload"]["jsonrpc"] = "2.0";
  doc["payload"]["id"] = id;
  doc["payload"]["result"]["tools"] = JsonArray();
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

}  // namespace xiaozhi
