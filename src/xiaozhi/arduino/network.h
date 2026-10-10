#pragma once
// Arduino: HTTPClient による OTA POST + 共通 TLS 設定。
// (旧 arduino_http + tls_bundle の統合)
#include <string>

#include <WiFiClientSecure.h>

struct HttpResult {
  int status = -1;
  std::string body;
  std::string error;
};

// TLS CA bundle (ESP-IDF x509_crt_bundle 形式) の共享ヘルパー。
// 生成: tools/gen_crt_bundle.py (参照: /references/AirSensors_LowPower/)
// 埋込: platformio.ini の board_build.embed_files = x509_crt_bundle
// 既定は正規 CA 検証あり。デバック時のみ -DCHATBOT_INSECURE_FALLBACK で
// setInsecure() にフォールバックする。
extern const uint8_t tls_crt_bundle_start[]
    asm("_binary_x509_crt_bundle_start");

inline void applyTlsBundle(WiFiClientSecure& c) {
#ifdef CHATBOT_INSECURE_FALLBACK
  c.setInsecure();
#else
  c.setCACertBundle(tls_crt_bundle_start);
#endif
}

// 同期 POST。timeout_ms は HTTPClient のタイムアウト。
HttpResult httpPost(const std::string& url, const std::string& body,
                    const std::string& device_id,
                    const std::string& client_id, int timeout_ms = 15000);