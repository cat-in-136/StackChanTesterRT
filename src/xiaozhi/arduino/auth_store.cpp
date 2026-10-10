#include "auth_store.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>

// 公式ファーム (xiaozhi-esp32) と同一 NVS namespace ("board"/"uuid")。
static const char* kNvsNamespace = "board";
static const char* kUuidKey = "uuid";

void AuthStore::begin() {
  // 公式ファームの bind 済み UUID を最優先。
  // 同一 namespace のため、NVS を消さずに書き換えた場合は引き継がれる.
  {
    Preferences p;
    p.begin(kNvsNamespace, true);
    uuid_ = p.isKey(kUuidKey) ? p.getString(kUuidKey, "").c_str() : "";
    p.end();
  }
  const char* src = kNvsNamespace;
  if (uuid_.empty()) {
    ensureClientId();
    src = "fresh";
  }
  M5_LOGI("auth: client-id %.36s src=%s", uuid_.c_str(), src);
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
  // 公式ファーム (xiaozhi-esp32) と同一 NVS namespace。
  // 同一デバイスで bind 済みなら bind 時の UUID を引き継ぐ。
  {
    Preferences p;
    p.begin(kNvsNamespace, true);
    uuid_ = p.isKey(kUuidKey) ? p.getString(kUuidKey, "").c_str() : "";
    p.end();
    if (!uuid_.empty()) return uuid_;
  }
  uuid_ = makeUuid();
  Preferences p;
  p.begin(kNvsNamespace, false);
  p.putString(kUuidKey, uuid_.c_str());
  p.end();
  return uuid_;
}

xiaozhi::DeviceIdentity AuthStore::identity() {
  xiaozhi::DeviceIdentity id;
  id.device_id = WiFi.macAddress().c_str();
  id.client_id = ensureClientId();
  return id;
}
