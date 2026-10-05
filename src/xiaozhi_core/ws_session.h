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

}  // namespace xiaozhi
