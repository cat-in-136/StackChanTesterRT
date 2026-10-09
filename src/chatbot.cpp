#include "chatbot.h"

#include <Avatar.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>

#include <ArduinoJson.h>

#include <cstring>

#include "xiaozhi_core/activation.h"
#include "xiaozhi_core/debug_text_channel.h"
#include "xiaozhi_core/voice_pipeline.h"
#include "xiaozhi_core/mqtt_session.h"
#include "xiaozhi_port/arduino_http.h"
#include "xiaozhi_port/arduino_mqtt.h"
#include "xiaozhi_port/auth_store.h"

extern m5avatar::Avatar avatar;

// ---- 内部状態 ----
namespace {
enum Phase : uint8_t {
  PhIdle = 0,
  PhWifi,
  PhOta,
  PhActivationWait,
  PhMqttConnect,
  PhHelloWait,
  PhReady,
};

AuthStore g_auth;
ArduinoMqtt g_mqtt;
xiaozhi::VoicePipeline g_pipe;
xiaozhi::TextFormat g_fmt = xiaozhi::TextFormat::A_DetectText;
xiaozhi::ServerHelloMqtt g_hello_mqtt;

bool g_active = false;
Phase g_phase = PhIdle;
uint32_t g_phase_since = 0;
uint32_t g_hello_sent_ms = 0;
uint32_t g_last_retry_ms = 0;
// hello無応答のまま切断された連続回数。上限で自動再接続を止める。
int g_hello_fail_count = 0;
const int kMaxHelloFails = 3;
int g_ota_attempts = 0;
const int kMaxOtaAttempts = 10;
const uint32_t kOtaRetryMs = 15000;
// activation ポーリング状態 (公式 ota.cc Activate / Web runActivation 互換)。
// 未bind時サーバは activation{code} を返すため、code有りの場合は
// activate が200になるまで MQTT 接続しない。
bool g_need_activation = false;
std::string g_activation_code;
uint32_t g_last_activate_ms = 0;
int g_activate_attempts = 0;
const int kMaxActivateAttempts = 20;
const uint32_t kActivateIntervalMs = 3000;

std::string g_last_user;
std::string g_last_assistant;
std::string g_last_emotion;
std::string g_last_error;
bool g_listen_started = false;

char s_status_buf[160];
char s_chat_buf[192];

const char* phaseStr(Phase p) {
  switch (p) {
    case PhIdle:
      return "Idle";
    case PhWifi:
      return "Wifi";
    case PhOta:
      return "Ota";
    case PhActivationWait:
      return "ActivationWait";
    case PhMqttConnect:
      return "MqttConnect";
    case PhHelloWait:
      return "HelloWait";
    case PhReady:
      return "Ready";
  }
  return "?";
}

void setPhase(Phase p) {
  g_phase = p;
  g_phase_since = millis();
}

void showText(const char* text) {
  // Avatar 吹き出しは ASCII 前提だが日本語も素通し (表示崩れは許容・PoC)。
  avatar.setSpeechText(text);
}

// scaffold の間は expression を変えない (Neutral のまま)。
// setExpression() は Avatar タスク停止中の呼び出しが禁止のため、
///将来有効化する場合は loop 稼働中 (Avatar task 起動後) のみ呼ぶこと。
void applyEmotion(const std::string& e) {
  (void)e;
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  return false;
}

// WiFi 接続 (設定経路は SCPI SYST:WIFI:NET のみ)。
// /references/AirSensors_LowPower/task_wifi.cpp::connectWifi 流用:
// wifi-config が空ならエラーログ、接続失敗時もエラーログ。
void startWifi() {
  Preferences p;
  p.begin("wifi-config", true);
  bool has_ssid = p.isKey("WIFI_SSID");
  bool has_pass = p.isKey("WIFI_PASSWD");
  String ssid = has_ssid ? p.getString("WIFI_SSID", "") : "";
  String pass = has_pass ? p.getString("WIFI_PASSWD", "") : "";
  p.end();
  if (ssid.isEmpty() && pass.isEmpty()) {
    g_last_error = "No WiFi Setting (SYST:WIFI:NET SSID,PASS)";
    M5_LOGE("chatbot: %s", g_last_error.c_str());
    Serial.println("CHAT WIFI-NG No WiFi Setting");
    showText("No WiFi");
    return;
  }
  M5_LOGI("chatbot: wifi begin ssid=%s", ssid.c_str());
  WiFi.disconnect(true);
  delay(1);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

// 接続情報はランタイム保持のみ (NVS 保存しない)。毎起動 OTA で取り直す。
// 制御チャネルは MQTT のみ (公式 application.cc:538-540 も mqtt 優先)。
static xiaozhi::MqttConfig g_mqtt_cfg;
static std::string g_mqtt_sub_topic;

bool hasRuntimeServer() { return !g_mqtt_cfg.endpoint.empty(); }

bool transportConnected() { return g_mqtt.isConnected(); }

void transportLoop() { g_mqtt.loop(); }

bool transportSend(const std::string& text) { return g_mqtt.sendText(text); }

void transportClose() { g_mqtt.close(); }

// POST {ota}activate を1回実行。200=成功, 202=待機, その他=失敗。
// 公式 ota.cc Activate / Web activate.ts 互換 (ボディは "{}" 簡易系)。
bool doActivateOnce() {
  xiaozhi::DeviceIdentity id = g_auth.identity();
  std::string url = xiaozhi::activateUrlFor(xiaozhi::DEFAULT_OTA_URL);
  HttpResult r = httpPost(url, xiaozhi::buildActivateBody(g_activation_code),
                          id.device_id, id.client_id);
  if (r.status == 200) {
    M5_LOGI("chatbot: activate ok");
    return true;
  }
  if (r.status == 202) {
    M5_LOGD("chatbot: activate 202 (waiting)");
    return false;
  }
  M5_LOGW("chatbot: activate status=%d err=%s", r.status, r.error.c_str());
  return false;
}

// TLS 検証には正しい時刻が必須。WiFi 接続後に一度だけ NTP 同期する。
// 失敗しても fatal にしない (OTA/MQTT リトライ時に再試行される)。
bool s_time_synced = false;

bool ensureTimeSync() {
  if (s_time_synced) return true;
  const char* tz = getenv("TZ");
  if (!tz) tz = "JST-9";
  configTzTime(tz, "time.cloudflare.com", "ntp.nict.jp");
  for (int i = 0; i < 16; i++) {
    struct tm tm_info;
    if (getLocalTime(&tm_info, 500) && tm_info.tm_year > (2024 - 1900)) {
      s_time_synced = true;
      M5_LOGI("chatbot: ntp ok %04d-%02d-%02d %02d:%02d:%02d",
              tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
              tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
      return true;
    }
  }
  M5_LOGW("chatbot: ntp sync failed, TLS verify may fail");
  return false;
}

void doOtaOnce() {
  xiaozhi::DeviceIdentity id = g_auth.identity();
  const std::string ota = xiaozhi::DEFAULT_OTA_URL;  // public server 固定
  std::string body = xiaozhi::buildCheckVersionBody(id);
  M5_LOGI("chatbot: POST %s", ota.c_str());
  HttpResult r = httpPost(ota, body, id.device_id, id.client_id);
  if (r.status != 200) {
    g_last_error = "ota http " + std::to_string(r.status) + " " + r.error;
    M5_LOGE("chatbot: %s", g_last_error.c_str());
    Serial.printf("CHAT OTA-NG %s\r\n", g_last_error.c_str());
    return;
  }
  xiaozhi::CheckVersionResult cv;
  if (!xiaozhi::parseCheckVersionBody(r.body, cv)) {
    g_last_error = "ota parse failed";
    M5_LOGE("chatbot: %s body=%.200s", g_last_error.c_str(), r.body.c_str());
    Serial.printf("CHAT OTA-NG %s\r\n", g_last_error.c_str());
    return;
  }
  // 診断: 応答 body と activation ブロックの中身を出す。
  // 未bind (activation code 有り) / bind済 (mqtt ブロック有り) の切り分け用。
  {
    std::string masked = r.body;
    size_t tp = masked.find("\"token\"");
    if (tp != std::string::npos) {
      size_t colon = masked.find(':', tp);
      size_t q1 = (colon == std::string::npos)
                      ? std::string::npos
                      : masked.find('"', colon + 1);
      if (q1 != std::string::npos) {
        size_t q2 = masked.find('"', q1 + 1);
        if (q2 != std::string::npos) masked.replace(q1 + 1, q2 - q1 - 1, "***");
      }
    }
    // M5_LOGD は抑制される可能性があるため Serial でも出す (実機ログで必ず見える)。
    M5_LOGI("chatbot: ota body %.500s", masked.c_str());
    Serial.printf("CHAT OTA-BODY %s\r\n", masked.c_str());
    Serial.printf("CHAT OTA-PARSE mqtt=%d code=%s challenge=%s timeout_ms=%d msg=%.120s\r\n",
                  (int)cv.mqtt.present,
                  cv.activation.code.empty() ? "(none)" : cv.activation.code.c_str(),
                  cv.activation.challenge.empty() ? "(none)"
                                                 : cv.activation.challenge.c_str(),
                  cv.activation.timeout_ms, cv.activation.message.c_str());
    M5_LOGI("chatbot: ota parsed mqtt=%d code=%s challenge=%s timeout_ms=%d msg=%.120s",
            (int)cv.mqtt.present,
            cv.activation.code.empty() ? "(none)" : cv.activation.code.c_str(),
            cv.activation.challenge.empty() ? "(none)"
                                           : cv.activation.challenge.c_str(),
            cv.activation.timeout_ms, cv.activation.message.c_str());
  }
  if (!cv.activation.code.empty()) {
    // 初回は xiaozhi.me 用の 6桁コードを画面表示する。
    char buf[64];
    snprintf(buf, sizeof(buf), "Code:%s", cv.activation.code.c_str());
    showText(buf);
    M5_LOGI("chatbot: activation %s %s", buf,
            cv.activation.message.c_str());
    Serial.printf("CHAT ACTIVATION code=%s msg=%s\r\n",
                  cv.activation.code.c_str(), cv.activation.message.c_str());
    // 未バインド: activate が200になるまで MQTT 接続しない。
    g_need_activation = true;
    g_activation_code = cv.activation.code;
    g_activate_attempts = 0;
    setPhase(PhActivationWait);
    g_last_retry_ms = millis();
    return;
  }
  g_need_activation = false;
  // 制御チャネルは MQTT のみ (公式 application.cc:538-540 も mqtt 優先)。
  if (cv.mqtt.present && !cv.mqtt.endpoint.empty()) {
    g_mqtt_cfg = cv.mqtt;
    g_mqtt_sub_topic = cv.mqtt.subscribe_topic;
    if (g_mqtt_sub_topic.empty() || g_mqtt_sub_topic == "null") {
      g_mqtt_sub_topic = "devices/p2p/";
      for (char c : id.device_id) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == ':') c = '_';
        g_mqtt_sub_topic += c;
      }
    }
    M5_LOGI("chatbot: ota ok, mqtt transport (endpoint=%s pub=%s sub=%s)",
            cv.mqtt.endpoint.c_str(), cv.mqtt.publish_topic.c_str(),
            g_mqtt_sub_topic.c_str());
    Serial.printf("CHAT OTA-TRANSPORT mqtt sub=%s\r\n",
                  g_mqtt_sub_topic.c_str());
    g_last_error.clear();
    setPhase(PhMqttConnect);
    return;
  }
  // websocket は test-token 固定ダミーのため使えない。mqtt 無しは異常系。
  g_last_error = "ota: no usable transport (mqtt missing)";
  M5_LOGE("chatbot: %s", g_last_error.c_str());
}

void connectMqtt() {
  if (g_mqtt_cfg.endpoint.empty()) {
    setPhase(PhOta);
    return;
  }
  M5_LOGI("chatbot: mqtt connect %s (client %s)", g_mqtt_cfg.endpoint.c_str(),
          g_mqtt_cfg.client_id.c_str());
  // on_open が sendHello -> PhHelloWait へ遷移させるため、
  // 先に PhMqttConnect を立ててから begin する (上書き防止)。
  setPhase(PhMqttConnect);
  g_last_retry_ms = millis();
  if (!g_mqtt.begin(g_mqtt_cfg, g_mqtt_sub_topic)) {
    if (g_phase == PhMqttConnect) {
      g_last_error = "mqtt connect failed";
      M5_LOGE("chatbot: %s", g_last_error.c_str());
    }
    return;
  }
}

void sendHello() {
  std::string hello = xiaozhi::buildMqttHello();
  g_pipe.startSession(millis());
  setPhase(PhHelloWait);
  g_hello_sent_ms = millis();
  if (!transportSend(hello)) {
    g_last_error = "hello send failed (not connected)";
    M5_LOGE("chatbot: %s", g_last_error.c_str());
  } else {
    M5_LOGI("chatbot: hello sent");
  }
  g_listen_started = false;
}

void setupPipeCallbacks() {
  // 同一 session_id 継続: tts/stop 後も Listening 維持 (Idle に落とさない)。
  g_pipe.tts_stop_returns_to_idle = false;
  g_pipe.cb.on_state = [](xiaozhi::PipelineState s) {
    M5_LOGI("chatbot: pipeline -> %s", xiaozhi::toStr(s));
  };
  g_pipe.cb.on_chat_text = [](const std::string& role,
                               const std::string& text) {
    if (role == "user") {
      g_last_user = text;
      M5_LOGI("chatbot: stt: %.120s", text.c_str());
      Serial.printf("CHAT STT %s\r\n", text.c_str());
    } else {
      g_last_assistant = text;
      showText(text.c_str());
      M5_LOGI("chatbot: tts sentence: %.120s", text.c_str());
      Serial.printf("CHAT TTS %s\r\n", text.c_str());
    }
  };
  g_pipe.cb.on_emotion = [](const std::string& e) {
    g_last_emotion = e;
    applyEmotion(e);
    Serial.printf("CHAT LLM emotion=%s\r\n", e.c_str());
  };
  g_pipe.cb.on_tts_start = []() {
    M5_LOGI("chatbot: tts start (uplink muted)");
    Serial.println("CHAT TTS-START");
  };
  g_pipe.cb.on_tts_stop = []() {
    M5_LOGI("chatbot: tts stop");
    Serial.println("CHAT TTS-STOP");
    // 次の TEXT で listen/start から送り直すため (同一 sid 継続)。
    g_listen_started = false;
  };
  g_pipe.cb.on_alert = [](const std::string& m) {
    M5_LOGI("chatbot: alert %.120s", m.c_str());
    Serial.printf("CHAT ALERT %s\r\n", m.c_str());
  };
  g_pipe.cb.on_goodbye = []() {
    // 公式: サーバ goodbye には返信しない。上位で Idle+close し再接続に任せる。
    M5_LOGI("chatbot: goodbye -> reconnect");
    Serial.println("CHAT GOODBYE");
    transportClose();
    g_listen_started = false;
    if (g_active) {
      setPhase(PhMqttConnect);
      g_last_retry_ms = millis();
    }
  };
  g_pipe.cb.on_reboot_request = []() {
    M5_LOGI("chatbot: reboot requested -> ignored in PoC (no reboot)");
    Serial.println("CHAT REBOOT-REQ ignored");
  };
  g_pipe.cb.on_timeout = []() {
    g_last_error = "120s rx timeout -> Idle";
    M5_LOGI("chatbot: %s", g_last_error.c_str());
    transportClose();
    setPhase(PhMqttConnect);
    g_last_retry_ms = millis();
  };
}

// MCP initialize / tools/list に応答する。それ以外の mcp (notifications 等) は無視。
// 戻り値: mcp メッセージだったか (hello 等への処理を継続しない)。
bool tryHandleMcp(const std::string& text) {
  JsonDocument doc;
  if (deserializeJson(doc, text.c_str())) return false;
  const char* type = doc["type"];
  if (!type || std::strcmp(type, "mcp") != 0) return false;
  // method/id は payload 配下・トップレベルの両対応。
  const char* method = nullptr;
  if (!doc["payload"].isNull() && !doc["payload"]["method"].isNull()) {
    method = doc["payload"]["method"];
  } else if (!doc["method"].isNull()) {
    method = doc["method"];
  }
  std::string method_s = method ? method : "";
  int id = 0;
  bool has_id = false;
  if (!doc["payload"].isNull() && !doc["payload"]["id"].isNull()) {
    id = doc["payload"]["id"].as<int>();
    has_id = true;
  } else if (!doc["id"].isNull()) {
    id = doc["id"].as<int>();
    has_id = true;
  }
  M5_LOGI("chatbot: mcp rx method=%s", method_s.empty() ? "(none)" : method_s.c_str());
  if (method_s == "initialize" && has_id) {
    std::string resp =
        xiaozhi::buildMcpInitializeResult(g_pipe.sessionId(), id);
    bool ok = transportSend(resp);
    M5_LOGI("chatbot: mcp initialize replied id=%d ok=%d", id, (int)ok);
  } else if (method_s == "tools/list" && has_id) {
    std::string resp =
        xiaozhi::buildMcpToolsListResult(g_pipe.sessionId(), id);
    bool ok = transportSend(resp);
    M5_LOGI("chatbot: mcp tools/list replied id=%d ok=%d", id, (int)ok);
  }
  return true;
}

void setupMqttCallbacks() {
  g_mqtt.ev.on_open = []() {
    M5_LOGI("chatbot: mqtt open");
    sendHello();
  };
  g_mqtt.ev.on_text = [](const std::string& text) {
    // 診断: 受信メッセージを全部ログ (MCP 無視分も含めサーバ応答を可視化)
    M5_LOGI("chatbot: mqtt rx %.200s", text.c_str());
    Serial.printf("CHAT MQTT-RX %s\r\n", text.c_str());
    uint32_t now = millis();
    if (tryHandleMcp(text)) return;
    // Connecting 中は server hello として解釈
    if (g_pipe.state() == xiaozhi::PipelineState::Connecting) {
      xiaozhi::ServerHelloMqtt h = xiaozhi::parseServerHelloMqtt(text);
      if (h.ok) {
        g_hello_mqtt = h;
        g_pipe.onHelloOk(h.session_id, now);
        g_hello_fail_count = 0;
        setPhase(PhReady);
        showText("Ready");
        M5_LOGI("chatbot: mqtt hello ok sid=%.16s udp=%s:%d",
                h.session_id.c_str(), h.udp_server.c_str(), h.udp_port);
        Serial.printf("CHAT HELLO-OK sid=%s sr=%d udp=%s:%d\r\n",
                      h.session_id.c_str(), h.sample_rate, h.udp_server.c_str(),
                      h.udp_port);
      } else {
        M5_LOGD("chatbot: ignore non-hello during Connecting: %.120s",
                text.c_str());
      }
      return;
    }
    xiaozhi::ServerEvent ev = g_pipe.onText(text, now);
    if (ev.kind == xiaozhi::ServerEvent::Kind::None) {
      M5_LOGD("chatbot: ignore rx: %.160s", text.c_str());
    }
  };
  g_mqtt.ev.on_close = []() {
    M5_LOGI("chatbot: mqtt closed");
    g_pipe.stopSession("close", millis());
    if (!g_active || g_phase == PhIdle) return;
    if (g_phase == PhHelloWait) {
      g_hello_fail_count++;
      if (g_hello_fail_count >= kMaxHelloFails) {
        g_last_error = "mqtt hello rejected";
        M5_LOGE("chatbot: %s x%d", g_last_error.c_str(), g_hello_fail_count);
        Serial.printf("CHAT MQTT-REJECT %s\r\n", g_last_error.c_str());
        showText("MQTT reject");
        setPhase(PhIdle);
        return;
      }
    }
    setPhase(PhMqttConnect);
    g_last_retry_ms = millis();
  };
  g_mqtt.ev.on_error = [](const std::string& msg) {
    M5_LOGW("chatbot: mqtt error %.160s", msg.c_str());
  };
}

}  // namespace

void setupChatbot(void) {
  g_active = true;
  g_auth.begin();
  setupPipeCallbacks();
  setupMqttCallbacks();
  // expression は変えない (scaffold につき Neutral のまま)。
  showText("Chatbot...");
  // 画面ログの抑止は changeOperationMode の共通処理に委譲
  // (Avatar 稼働中は display ターゲットを ESP_LOG_NONE にする)。
  M5_LOGI("chatbot: setup ota=%s", xiaozhi::DEFAULT_OTA_URL);
  g_mqtt_cfg = xiaozhi::MqttConfig{};
  g_mqtt_sub_topic.clear();
  if (!ensureWifi()) {
    startWifi();
    setPhase(PhWifi);
  } else {
    ensureTimeSync();  // 起動時に接続済みの場合も TLS 検証用に同期
    setPhase(PhOta);  // 毎起動 OTA で接続情報取得 (NVS 保存しない)
  }
  g_ota_attempts = 0;
  g_need_activation = false;
  g_activation_code.clear();
  g_activate_attempts = 0;
  g_last_error.clear();
  g_last_user.clear();
  g_last_assistant.clear();
}

void teardownChatbot(void) {
  g_active = false;
  transportClose();
  g_pipe.abort(millis());
  setPhase(PhIdle);
  WiFi.disconnect(true);
  M5_LOGI("chatbot: teardown");
}

void loopChatbot(void) {
  if (!g_active) return;
  transportLoop();
  // now は on_open/on_close 等のコールバック実行後に取得する。
  // loop前の古いnowで引き算すると、コールバック内で更新された時刻
  // (g_hello_sent_ms / g_last_retry_ms) より小さくなり、符号なし
  // アンダーフローで hello即timeout / 即再接続を誤発火する。
  uint32_t now = millis();
  g_pipe.checkTimeout(now);

  switch (g_phase) {
    case PhWifi:
      if (WiFi.status() == WL_CONNECTED) {
        M5_LOGI("chatbot: wifi ok %s", WiFi.localIP().toString().c_str());
        Serial.printf("CHAT WIFI-OK %s\r\n",
                      WiFi.localIP().toString().c_str());
        showText("WiFi OK");
        ensureTimeSync();  // TLS 検証用 (失敗時は警告のみで続行)
        setPhase(PhOta);
      } else if (now - g_phase_since > 20000u) {
        g_last_error = "wifi connect failed";
        M5_LOGE("chatbot: %s (SYST:WIFI:NET to set)", g_last_error.c_str());
        Serial.println("CHAT WIFI-NG connect failed");
        showText("WiFi NG");
        setPhase(PhIdle);
      }
      break;
    case PhOta:
      doOtaOnce();
      g_ota_attempts++;
      if (g_phase == PhOta) {  // まだ Ota ならリトライ待機へ
        if (g_ota_attempts >= kMaxOtaAttempts) {
          g_last_error = "ota max retries";
          setPhase(PhIdle);
        } else {
          setPhase(PhActivationWait);
          g_last_retry_ms = now;
        }
      }
      break;
    case PhActivationWait:
      if (g_need_activation) {
        if (now - g_last_activate_ms > kActivateIntervalMs) {
          g_last_activate_ms = now;
          g_activate_attempts++;
          if (doActivateOnce()) {
            g_need_activation = false;
            g_activation_code.clear();
            g_ota_attempts = 0;
            setPhase(PhOta);
          } else if (g_activate_attempts >= kMaxActivateAttempts) {
            g_last_error = "activation timeout (bind at xiaozhi.me?)";
            M5_LOGE("chatbot: %s", g_last_error.c_str());
            setPhase(PhIdle);
          }
        }
        break;
      }
      if (now - g_last_retry_ms > kOtaRetryMs) {
        if (g_ota_attempts >= kMaxOtaAttempts) {
          g_last_error = "activation timeout (bind at xiaozhi.me?)";
          M5_LOGE("chatbot: %s", g_last_error.c_str());
          setPhase(PhIdle);
        } else {
          g_ota_attempts++;
          setPhase(PhOta);
        }
      } else if (g_phase == PhActivationWait && hasRuntimeServer() &&
                 !transportConnected()) {
        connectMqtt();
      }
      break;
    case PhMqttConnect:
      if (!transportConnected() && now - g_last_retry_ms > 5000u) {
        connectMqtt();
      }
      break;
    case PhHelloWait:
      if (now - g_hello_sent_ms > (uint32_t)xiaozhi::kHelloTimeoutMs) {
        g_last_error = "hello timeout";
        M5_LOGE("chatbot: %s", g_last_error.c_str());
        g_pipe.onHelloFailed(now);
        g_hello_fail_count++;
        if (g_hello_fail_count >= kMaxHelloFails) {
          g_last_error = "mqtt hello rejected";
          M5_LOGE("chatbot: %s x%d", g_last_error.c_str(), g_hello_fail_count);
          Serial.printf("CHAT MQTT-REJECT %s\r\n", g_last_error.c_str());
          showText("MQTT reject");
          setPhase(PhIdle);
          break;
        }
        transportClose();
        setPhase(PhMqttConnect);
        g_last_retry_ms = now;
      }
      break;
    case PhReady:
    case PhIdle:
      break;
  }
}

bool chatbotSendText(const char* text) {
  if (!g_active || !text || !text[0]) return false;
  // 同一 session_id 継続: Idle でも Ready+接続中なら送信可とする。
  // (tts/stop 後は Listening 維持が既定だが、ABORT/STOP 直後の Idle 復帰用)。
  // hello 未完了 (Connecting) のみ拒否する。
  if (g_phase != PhReady ||
      g_pipe.state() == xiaozhi::PipelineState::Connecting) {
    g_last_error = "not ready (hello not done)";
    return false;
  }
  if (!transportConnected()) {
    g_last_error = "not ready (transport disconnected)";
    return false;
  }
  const std::string& sid = g_pipe.sessionId();
  if (sid.empty()) {
    g_last_error = "not ready (no session)";
    return false;
  }
  uint32_t now = millis();
  if (!g_listen_started) {
    std::string start =
        xiaozhi::buildListenStart(sid, "manual");  // manual 固定 (plan)
    if (!transportSend(start)) return false;
    g_listen_started = true;
  }
  std::string msg = xiaozhi::buildBypassText(sid, text, g_fmt);
  bool ok = transportSend(msg);
  if (ok) {
    M5_LOGI("chatbot: tx text fmt=%s %.120s",
            g_fmt == xiaozhi::TextFormat::A_DetectText ? "A" : "B", text);
  }
  (void)now;
  return ok;
}

void chatbotStart(void) {
  if (!g_active) return;
  g_hello_fail_count = 0;  // 手動リトライでは失敗カウンタをリセット
  if (!ensureWifi()) {
    startWifi();
    setPhase(PhWifi);
    return;
  }
  if (!hasRuntimeServer()) {
    g_ota_attempts = 0;
    setPhase(PhOta);  // 毎起動 OTA (保存しない)
    return;
  }
  if (!transportConnected()) {
    connectMqtt();
    return;
  }
  // 公式は毎回 OpenAudioChannel->hello で session 更新。旧 sid 再利用はしない。
  // Idle なら再接続からやり直す。
  if (g_pipe.state() == xiaozhi::PipelineState::Idle) {
    connectMqtt();
    return;
  }
}

void chatbotStop(void) {
  if (g_pipe.sessionId().empty()) return;
  std::string s = xiaozhi::buildListenStop(g_pipe.sessionId());
  transportSend(s);
  g_pipe.stopSession("stop", millis());
  g_listen_started = false;
}

void chatbotAbort(void) {
  if (!g_pipe.sessionId().empty()) {
    std::string s = xiaozhi::buildAbort(g_pipe.sessionId());
    transportSend(s);
  }
  g_pipe.abort(millis());
  transportClose();
  g_listen_started = false;
  if (g_active) {
    setPhase(PhMqttConnect);
    g_last_retry_ms = millis();
  }
}

const char* chatbotStatus(void) {
  snprintf(s_status_buf, sizeof(s_status_buf), "phase=%s pipe=%s wifi=%d fmt=%s",
           phaseStr(g_phase), xiaozhi::toStr(g_pipe.state()),
           (int)WiFi.status(),
           g_fmt == xiaozhi::TextFormat::A_DetectText ? "A" : "B");
  return s_status_buf;
}

void chatbotSetFormatB(bool use_b) {
  g_fmt = use_b ? xiaozhi::TextFormat::B_TypeText
                : xiaozhi::TextFormat::A_DetectText;
}
bool chatbotGetFormatB(void) {
  return g_fmt == xiaozhi::TextFormat::B_TypeText;
}

const char* chatbotLastChat(void) {
  snprintf(s_chat_buf, sizeof(s_chat_buf), "user=%.60s asm=%.60s emo=%.20s err=%.40s",
           g_last_user.c_str(), g_last_assistant.c_str(),
           g_last_emotion.c_str(), g_last_error.c_str());
  return s_chat_buf;
}
