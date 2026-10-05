#pragma once
// DebugTextChannel: STTバイパス入力 (plan §4)。送信形式を IF 化し A/B 切替。
#include <string>

#include "json_min.h"
#include "types.h"

namespace xiaozhi {

// A (既定・互換優先): listen/detect+text (protocol.cc:75-80 流用)
// B (サーバ拡張用): type:text
inline std::string buildBypassText(const std::string& session_id,
                                   const std::string& text, TextFormat fmt) {
  std::string sid = json_min::escape(session_id);
  std::string t = json_min::escape(text);
  if (fmt == TextFormat::B_TypeText) {
    return "{\"session_id\":\"" + sid + "\",\"type\":\"text\",\"text\":\"" +
           t + "\"}";
  }
  return "{\"session_id\":\"" + sid +
         "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + t + "\"}";
}

inline std::string buildListenStart(const std::string& session_id,
                                    const std::string& mode = "manual") {
  return "{\"session_id\":\"" + json_min::escape(session_id) +
         "\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"" +
         json_min::escape(mode) + "\"}";
}

inline std::string buildListenStop(const std::string& session_id) {
  return "{\"session_id\":\"" + json_min::escape(session_id) +
         "\",\"type\":\"listen\",\"state\":\"stop\"}";
}

inline std::string buildAbort(const std::string& session_id,
                              const std::string& reason = "") {
  std::string o =
      "{\"session_id\":\"" + json_min::escape(session_id) + "\"";
  // abort は plan §1 D->S 状態通知のみ。reason 省略可。
  (void)reason;
  o += ",\"type\":\"abort\"}";
  return o;
}

}  // namespace xiaozhi
