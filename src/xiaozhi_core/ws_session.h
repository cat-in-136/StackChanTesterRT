#pragma once
// WsSession: hello 組立・server hello 解析・10s待ち定数 (plan §3.1)
#include <string>

#include "json_min.h"
#include "types.h"

namespace xiaozhi {

// public server 固定。設定不要 (OTA URL の切替はビルド時定数で)。
static const char DEFAULT_OTA_URL[] = "https://api.tenclass.net/xiaozhi/ota/";
static const int kHelloTimeoutMs = 10000;

// D->S hello (v1 固定)。公式は features.mcp:true が必須級。
inline std::string buildHello() {
  return "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},"
         "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
         "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
}

// D->S hello (MQTT 制御チャネル用: transport=udp, version=3)。
// 公式 mqtt_protocol.cc GetHelloMessage 互換。UDP は後段で実装予定。
inline std::string buildMqttHello() {
  return "{\"type\":\"hello\",\"version\":3,\"features\":{\"mcp\":true},"
         "\"transport\":\"udp\",\"audio_params\":{\"format\":\"opus\","
         "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
}

struct ServerHello {
  std::string session_id;  // 公式は任意。空でも受理する。
  int sample_rate = 24000;  // 下り既定 (protocol.h:79)
  int frame_duration = 60;
};

// type==hello && transport==websocket のみ受理。session_id 欠落でも true。
// (公式 websocket_protocol.cc:236-254 は sid 無しでも event set する)
inline bool parseServerHello(const std::string& text, ServerHello& out) {
  std::string type, transport;
  if (!json_min::extractString(text, "type", type)) return false;
  if (type != "hello") return false;
  if (!json_min::extractString(text, "transport", transport)) return false;
  if (transport != "websocket") return false;
  // session_id は任意 (公式は保存のみ)。欠落・空でも受理する。
  json_min::extractString(text, "session_id", out.session_id);
  int sr = 0, fd = 0;
  if (json_min::extractInt(text, "sample_rate", sr)) out.sample_rate = sr;
  if (json_min::extractInt(text, "frame_duration", fd) && fd > 0)
    out.frame_duration = fd;
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
  std::string type, transport;
  if (!json_min::extractString(text, "type", type)) return h;
  if (type != "hello") return h;
  if (!json_min::extractString(text, "transport", transport)) return h;
  if (transport != "udp") return h;
  h.ok = true;
  json_min::extractString(text, "session_id", h.session_id);
  int sr = 0, fd = 0;
  if (json_min::extractInt(text, "sample_rate", sr)) h.sample_rate = sr;
  if (json_min::extractInt(text, "frame_duration", fd) && fd > 0)
    h.frame_duration = fd;
  std::string scope;
  if (json_min::jsonScope(text, "udp", scope)) {
    json_min::extractString(scope, "server", h.udp_server);
    int p = 0;
    if (json_min::extractInt(scope, "port", p)) h.udp_port = p;
    json_min::extractString(scope, "key", h.udp_key);
    json_min::extractString(scope, "nonce", h.udp_nonce);
  }
  return h;
}

// MCP initialize への応答 (公式 mcp_server.cc:417-430 互換)。
inline std::string buildMcpInitializeResult(const std::string& session_id,
                                            int id) {
  std::string m = "{\"session_id\":\"" + json_min::escape(session_id) +
                  "\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" +
                  std::to_string(id) +
                  ",\"result\":{\"protocolVersion\":\"2024-11-05\","
                  "\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":"
                  "\"m5stack-cores3\",\"version\":\"0.1.0\"}}}}";
  return m;
}

// MCP tools/list への応答 (空ツール一覧)。
inline std::string buildMcpToolsListResult(const std::string& session_id,
                                           int id) {
  std::string m = "{\"session_id\":\"" + json_min::escape(session_id) +
                  "\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" +
                  std::to_string(id) + ",\"result\":{\"tools\":[]}}}";
  return m;
}

}  // namespace xiaozhi
