#pragma once
// ports/arduino-cores3: AuthStore (plan §5)
// NVS 保存は uuid (Client-Id) のみ。公式ファーム (xiaozhi-esp32 board.cc) と
// 同一 namespace "board"/"uuid" を使い、bind 済み UUID を引き継ぐ。
// OTA URL は public server 固定 (xiaozhi::DEFAULT_OTA_URL)、websocket url/token
// は毎起動 OTA で取得しランタイム保持のみ (NVS 保存しない)。
#include <string>

#include "../xiaozhi_core/types.h"

class AuthStore {
 public:
  void begin();
  // 初回はランダム生成して保存
  std::string ensureClientId();
  xiaozhi::DeviceIdentity identity();

 private:
  std::string uuid_;
  std::string makeUuid();
};
