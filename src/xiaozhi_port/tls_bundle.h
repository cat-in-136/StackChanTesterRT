#pragma once
// TLS CA bundle (ESP-IDF x509_crt_bundle 形式) の共有ヘルパー。
// 生成: tools/gen_crt_bundle.py (参照: /references/AirSensors_LowPower/)
// 埋込: platformio.ini の board_build.embed_files = x509_crt_bundle
// 既定は正規 CA 検証あり。デバッグ時のみ -DCHATBOT_INSECURE_FALLBACK で
// setInsecure() にフォールバックする。
#include <WiFiClientSecure.h>
#include <stdint.h>

extern const uint8_t tls_crt_bundle_start[]
    asm("_binary_x509_crt_bundle_start");

inline void applyTlsBundle(WiFiClientSecure& c) {
#ifdef CHATBOT_INSECURE_FALLBACK
  c.setInsecure();
#else
  c.setCACertBundle(tls_crt_bundle_start);
#endif
}
