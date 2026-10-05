#pragma once
// ports: Arduino HTTPClient による HttpClientIF 相当 (OTA POST 用)
#include <string>

struct HttpResult {
  int status = -1;
  std::string body;
  std::string error;
};

// 同期 POST。timeout_ms は HTTPClient のタイムアウト。
HttpResult httpPost(const std::string& url, const std::string& body,
                    const std::string& device_id,
                    const std::string& client_id, int timeout_ms = 15000);
