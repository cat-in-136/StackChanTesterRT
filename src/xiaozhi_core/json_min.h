#pragma once
// 最小 JSON 補助 (core 用)。ArduinoJson / cJSON に依存しない。
// フラットな string / int 抽出のみ。ネストはキー名で横断検索する。
#include <cctype>
#include <string>

namespace xiaozhi {
namespace json_min {

inline std::string escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        o += c;
        break;
    }
  }
  return o;
}

// "key" を探し、その後の : 以降の文字列値を取り出す。\" エスケープ対応。
inline bool extractString(const std::string& json, const char* key,
                          std::string& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + pat.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[pos])))
    pos++;
  if (pos >= json.size() || json[pos] != '"') return false;
  pos++;
  std::string o;
  while (pos < json.size()) {
    char c = json[pos];
    if (c == '\\' && pos + 1 < json.size()) {
      char n = json[pos + 1];
      switch (n) {
        case '"':
          o += '"';
          break;
        case '\\':
          o += '\\';
          break;
        case 'n':
          o += '\n';
          break;
        case 'r':
          o += '\r';
          break;
        case 't':
          o += '\t';
          break;
        default:
          o += n;
          break;
      }
      pos += 2;
      continue;
    }
    if (c == '"') {
      out = o;
      return true;
    }
    o += c;
    pos++;
  }
  return false;
}

inline bool extractInt(const std::string& json, const char* key, int& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + pat.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < json.size() &&
         (std::isspace(static_cast<unsigned char>(json[pos])) ||
          json[pos] == '"'))
    pos++;
  bool neg = false;
  if (pos < json.size() && (json[pos] == '-' || json[pos] == '+')) {
    neg = (json[pos] == '-');
    pos++;
  }
  if (pos >= json.size() ||
      !std::isdigit(static_cast<unsigned char>(json[pos])))
    return false;
  long v = 0;
  while (pos < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[pos]))) {
    v = v * 10 + (json[pos] - '0');
    pos++;
  }
  out = neg ? (int)-v : (int)v;
  return true;
}

}  // namespace json_min
}  // namespace xiaozhi
