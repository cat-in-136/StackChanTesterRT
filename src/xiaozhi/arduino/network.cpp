#include "network.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

HttpResult httpPost(const std::string& url, const std::string& body,
                    const std::string& device_id,
                    const std::string& client_id, int timeout_ms) {
  HttpResult r;
  bool is_https = url.rfind("https://", 0) == 0;
  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;
  http.setTimeout(timeout_ms);
  bool begun = false;
  if (is_https) {
    // 正規 CA 検証 (x509_crt_bundle 埋込)。http:// 自前 OTA は平文のまま。
    applyTlsBundle(secure);
    begun = http.begin(secure, url.c_str());
  } else {
    begun = http.begin(plain, url.c_str());
  }
  if (!begun) {
    r.error = "http.begin failed";
    return r;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Activation-Version", "1");
  http.addHeader("Device-Id", device_id.c_str());
  http.addHeader("Client-Id", client_id.c_str());
  http.addHeader("User-Agent", "StackChanRT-chatbot/0.1.0");
  http.addHeader("Accept-Language", "zh-CN");
  int code = http.POST(String(body.c_str()));
  r.status = code;
  if (code > 0) {
    String payload = http.getString();
    r.body = payload.c_str();
  } else {
    r.error = http.errorToString(code).c_str();
  }
  http.end();
  return r;
}