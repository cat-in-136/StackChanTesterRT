#include "auth_store.h"

#include <Preferences.h>
#include <WiFi.h>

static const char* kNs = "xz-min";

void AuthStore::begin() {
  Preferences p;
  p.begin(kNs, true);
  // 初回はキーが存在せず getString() が NOT_FOUND エラーログを出すため
  // isKey() でガードする。無い場合は空文字扱い (正常系)。
  uuid_ = p.isKey("uuid") ? p.getString("uuid", "").c_str() : "";
  p.end();
  if (uuid_.empty()) {
    ensureClientId();
  }
}

std::string AuthStore::makeUuid() {
  // UUIDv4 風 (esp_random 由来)。厳密な一意性より NVS 永続化が要点。
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  uint32_t c = esp_random();
  uint32_t d = esp_random();
  char buf[40];
  snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-%04x-%08x%04x", (unsigned)a,
           (unsigned)(b >> 16), (unsigned)(b & 0xfff),
           (unsigned)((c >> 16) & 0x3fff | 0x8000), (unsigned)c,
           (unsigned)(d & 0xffff));
  return std::string(buf);
}

std::string AuthStore::ensureClientId() {
  if (!uuid_.empty()) return uuid_;
  uuid_ = makeUuid();
  Preferences p;
  p.begin(kNs, false);
  p.putString("uuid", uuid_.c_str());
  p.end();
  return uuid_;
}

xiaozhi::DeviceIdentity AuthStore::identity() {
  xiaozhi::DeviceIdentity id;
  id.device_id = WiFi.macAddress().c_str();
  id.client_id = ensureClientId();
  return id;
}
