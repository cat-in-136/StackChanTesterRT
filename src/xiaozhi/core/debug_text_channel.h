#pragma once
// DebugTextChannel: STTバイパス入力 (plan §4)。送信形式を IF 化し A/B 切替。
#include <ArduinoJson.h>

#include <string>

#include "types.h"

namespace xiaozhi {

// A (既定・互換優先): listen/detect+text (protocol.cc:75-80 流用)
// B (サーバ拡張用): type:text
inline std::string buildBypassText(const std::string& session_id,
                                   const std::string& text, TextFormat fmt) {
  JsonDocument doc;
  doc["session_id"] = session_id;
  if (fmt == TextFormat::B_TypeText) {
    doc["type"] = "text";
    doc["text"] = text;
  } else {
    doc["type"] = "listen";
    doc["state"] = "detect";
    doc["text"] = text;
  }
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

inline std::string buildListenStart(const std::string& session_id,
                                    const std::string& mode = "manual") {
  JsonDocument doc;
  doc["session_id"] = session_id;
  doc["type"] = "listen";
  doc["state"] = "start";
  doc["mode"] = mode;
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

inline std::string buildListenStop(const std::string& session_id) {
  JsonDocument doc;
  doc["session_id"] = session_id;
  doc["type"] = "listen";
  doc["state"] = "stop";
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

inline std::string buildAbort(const std::string& session_id,
                              const std::string& reason = "") {
  JsonDocument doc;
  doc["session_id"] = session_id;
  doc["type"] = "abort";
  // abort は plan §1 D->S 状態通知のみ。reason 省略可。
  (void)reason;
  String out;
  serializeJson(doc, out);
  return std::string(out.c_str());
}

}  // namespace xiaozhi
