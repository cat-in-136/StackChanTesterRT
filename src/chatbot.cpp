#include "chatbot.h"

#include <Avatar.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>

#include "xiaozhi_core/activation.h"
#include "xiaozhi_core/debug_text_channel.h"
#include "xiaozhi_core/voice_pipeline.h"
#include "xiaozhi_core/ws_session.h"
#include "xiaozhi_port/arduino_http.h"
#include "xiaozhi_port/arduino_mqtt.h"
#include "xiaozhi_port/arduino_ws.h"
#include "xiaozhi_port/auth_store.h"

extern m5avatar::Avatar avatar;

// ---- 内部状態 ----
namespace {
enum Phase : uint8_t {
  PhIdle = 0,
  PhWifi,
  PhOta,
  PhActivationWait,
  PhWsConnect,
  PhHelloWait,
  PhReady,
};

AuthStore g_auth;
ArduinoWs g_ws;
ArduinoMqtt g_mqtt;
xiaozhi::VoicePipeline g_pipe;
xiaozhi::TextFormat g_fmt = xiaozhi::TextFormat::A_DetectText;
xiaozhi::ServerHello g_hello;
xiaozhi::ServerHelloMqtt g_hello_mqtt;

bool g_active = false;
Phase g_phase = PhIdle;
uint32_t g_phase_since = 0;
uint32_t g_hello_sent_ms = 0;
uint32_t g_last_retry_ms = 0;
// hello無応答のまま切断された連続回数。上限で自動再接続を止める
// (test-token 等の無効クレデンシャルで無限ループしないため)。
int g_hello_fail_count = 0;
const int kMaxHelloFails = 3;
int g_ota_attempts = 0;
const int kMaxOtaAttempts = 10;
const uint32_t kOtaRetryMs = 15000;
// activation ポーリング状態 (公式 ota.cc Activate / Web runActivation 互換)。
// 未bind時サーバは websocket(test-token仮値)+activation{code} を同梱して返すため、
// code有りの場合は websocket有無に関わらず activate が200になるまでWS接続しない。
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
    case PhWsConnect:
      return "WsConnect";
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
// 公式は mqtt ブロック優先 (application.cc:538-540) のため、
// mqtt があれば MQTT 制御チャネル、無ければ websocket を使う。
static std::string g_runtime_ws_url;
static std::string g_runtime_ws_token;
static xiaozhi::MqttConfig g_mqtt_cfg;
static std::string g_mqtt_sub_topic;

enum Transport : uint8_t { TpNone = 0, TpWs, TpMqtt };
static Transport g_transport = TpNone;

bool hasRuntimeServer() {
  return g_transport == TpMqtt || !g_runtime_ws_url.empty();
}

bool transportConnected() {
  return g_transport == TpMqtt ? g_mqtt.isConnected() : g_ws.isConnected();
}

void transportLoop() {
  if (g_transport == TpMqtt)
    g_mqtt.loop();
  else
    g_ws.loop();
}

bool transportSend(const std::string& text) {
  return g_transport == TpMqtt ? g_mqtt.sendText(text) : g_ws.sendText(text);
}

void transportClose() {
  if (g_transport == TpMqtt)
    g_mqtt.close();
  else
    g_ws.close();
}

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
// 失敗しても fatal にしない (OTA/WSS リトライ時に再試行される)。
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
  // 診断: token 値をマスクして応答 body を出す。
  // 未bind (activationのみ) / bind済 (websocket+token) の切り分け用。
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
    // activation ブロックの中身 (code/challenge/message) と token 実値を全部ログ。
    // token はデバイス自身の資格情報であり、ダミー(test-token)か実値かの
    // 切り分けに必須のため PoC 診断ログではマスクしない。
    const std::string& tok = cv.server.token;
    Serial.printf("CHAT OTA-PARSE ws=%d token=%s code=%s challenge=%s timeout_ms=%d msg=%.120s\r\n",
                  (int)cv.has_websocket,
                  tok.empty() ? "(none)" : tok.c_str(),
                  cv.activation.code.empty() ? "(none)" : cv.activation.code.c_str(),
                  cv.activation.challenge.empty() ? "(none)"
                                                 : cv.activation.challenge.c_str(),
                  cv.activation.timeout_ms, cv.activation.message.c_str());
    M5_LOGI("chatbot: ota parsed ws=%d token=%s code=%s challenge=%s timeout_ms=%d msg=%.120s",
            (int)cv.has_websocket,
            tok.empty() ? "(none)" : tok.c_str(),
            cv.activation.code.empty() ? "(none)" : cv.activation.code.c_str(),
            cv.activation.challenge.empty() ? "(none)"
                                           : cv.activation.challenge.c_str(),
            cv.activation.timeout_ms, cv.activation.message.c_str());
  }
  if (!cv.activation.code.empty()) {
    // 初回 (token 無し) は xiaozhi.me 用の 6桁コードを画面表示する。
    char buf[64];
    snprintf(buf, sizeof(buf), "Code:%s", cv.activation.code.c_str());
    showText(buf);
    M5_LOGI("chatbot: activation %s %s", buf,
            cv.activation.message.c_str());
    Serial.printf("CHAT ACTIVATION code=%s msg=%s\r\n",
                  cv.activation.code.c_str(), cv.activation.message.c_str());
    // 未バインド: websocket 有無に関わらず activate が200になるまで待つ。
    // (公式 ota.cc:502 / Web activate.ts: サーバは code 提示中も仮 token を
    // 同梱するため、websocket 有りでも test-token のまま接続しては無言切断される)
    g_need_activation = true;
    g_activation_code = cv.activation.code;
    g_activate_attempts = 0;
    setPhase(PhActivationWait);
    g_last_retry_ms = millis();
    return;
  }
  g_need_activation = false;
  // トランスポート選択: 公式は mqtt ブロック優先 (application.cc:538-540)。
  // mqtt があれば MQTT 制御チャネル (websocket.token は test-token のダミーで
  // 実接続不可のため、mqtt が無い場合のみ websocket にフォールバック)。
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
    g_transport = TpMqtt;
    M5_LOGI("chatbot: ota ok, mqtt transport (endpoint=%s pub=%s sub=%s)",
            cv.mqtt.endpoint.c_str(), cv.mqtt.publish_topic.c_str(),
            g_mqtt_sub_topic.c_str());
    Serial.printf("CHAT OTA-TRANSPORT mqtt sub=%s\r\n",
                  g_mqtt_sub_topic.c_str());
    g_last_error.clear();
    setPhase(PhWsConnect);
    return;
  }
  if (cv.has_websocket) {
    g_runtime_ws_url = cv.server.url;
    g_runtime_ws_token = cv.server.token;
    g_transport = TpWs;
    M5_LOGI("chatbot: ota ok, websocket transport (no mqtt block)");
    Serial.println("CHAT OTA-TRANSPORT websocket");
    g_last_error.clear();
    setPhase(PhWsConnect);
    return;
  }
  g_last_error = "ota: no usable transport";
  M5_LOGE("chatbot: %s", g_last_error.c_str());
}

void connectWs() {
  xiaozhi::DeviceIdentity id = g_auth.identity();
  if (g_transport == TpMqtt) {
    if (g_mqtt_cfg.endpoint.empty()) {
      setPhase(PhOta);
      return;
    }
    M5_LOGI("chatbot: mqtt connect %s (client %s)", g_mqtt_cfg.endpoint.c_str(),
            g_mqtt_cfg.client_id.c_str());
    // on_open が sendHello -> PhHelloWait へ遷移させるため、
    // 先に PhWsConnect を立ててから begin する (上書き防止)。
    setPhase(PhWsConnect);
    g_last_retry_ms = millis();
    if (!g_mqtt.begin(g_mqtt_cfg, g_mqtt_sub_topic)) {
      if (g_phase == PhWsConnect) {
        g_last_error = "mqtt connect failed";
        M5_LOGE("chatbot: %s", g_last_error.c_str());
      }
      return;
    }
    return;
  }
  if (g_runtime_ws_url.empty()) {
    setPhase(PhOta);
    return;
  }
  M5_LOGI("chatbot: wss connect %.60s...", g_runtime_ws_url.c_str());
  // on_open (非同期) が sendHello -> PhHelloWait へ遷移させるため、
  // 先に PhWsConnect を立ててから begin する (上書き防止)。
  setPhase(PhWsConnect);
  g_last_retry_ms = millis();
  if (!g_ws.begin(g_runtime_ws_url, g_runtime_ws_token, id.device_id,
                  id.client_id)) {
    // ArduinoWs::begin 失敗 = URL parse失敗。再試行は loop 側。
    if (g_phase == PhWsConnect) {
      g_last_error = "wss connect failed";
      M5_LOGE("chatbot: %s", g_last_error.c_str());
    }
    return;
  }
}

void sendHello() {
  std::string hello = (g_transport == TpMqtt) ? xiaozhi::buildMqttHello()
                                              : xiaozhi::buildHello();
  g_pipe.startSession(millis());
  setPhase(PhHelloWait);
  g_hello_sent_ms = millis();
  if (!transportSend(hello)) {
    g_last_error = "hello send failed (not connected)";
    M5_LOGE("chatbot: %s", g_last_error.c_str());
  } else {
    M5_LOGI("chatbot: hello sent (%s)", g_transport == TpMqtt ? "mqtt" : "ws");
  }
  g_listen_started = false;
}

void setupPipeCallbacks() {
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
      setPhase(PhWsConnect);
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
    setPhase(PhWsConnect);
    g_last_retry_ms = millis();
  };
}

// MCP initialize / tools/list に応答する。それ以外の mcp (notifications 等) は無視。
// 戻り値: mcp メッセージだったか (hello 等への処理を継続しない)。
bool tryHandleMcp(const std::string& text) {
  std::string type;
  if (!xiaozhi::json_min::extractString(text, "type", type)) return false;
  if (type != "mcp") return false;
  std::string method;
  xiaozhi::json_min::extractString(text, "method", method);
  int id = 0;
  bool has_id = xiaozhi::json_min::extractInt(text, "id", id);
  M5_LOGI("chatbot: mcp rx method=%s", method.empty() ? "(none)" : method.c_str());
  if (method == "initialize" && has_id) {
    std::string resp =
        xiaozhi::buildMcpInitializeResult(g_pipe.sessionId(), id);
    bool ok = transportSend(resp);
    M5_LOGI("chatbot: mcp initialize replied id=%d ok=%d", id, (int)ok);
  } else if (method == "tools/list" && has_id) {
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
        Serial.printf("CHAT WS-REJECT %s\r\n", g_last_error.c_str());
        showText("WS reject");
        setPhase(PhIdle);
        return;
      }
    }
    setPhase(PhWsConnect);
    g_last_retry_ms = millis();
  };
  g_mqtt.ev.on_error = [](const std::string& msg) {
    M5_LOGW("chatbot: mqtt error %.160s", msg.c_str());
  };
}

void setupWsCallbacks() {
  g_ws.ev.on_open = []() {
    M5_LOGI("chatbot: ws open");
    sendHello();
  };
  g_ws.ev.on_text = [](const std::string& text) {
    uint32_t now = millis();
    // Connecting 中は server hello として解釈
    if (g_pipe.state() == xiaozhi::PipelineState::Connecting) {
      xiaozhi::ServerHello h;
      if (xiaozhi::parseServerHello(text, h)) {
        g_hello = h;
        g_pipe.onHelloOk(h.session_id, now);
        g_hello_fail_count = 0;
        setPhase(PhReady);
        showText("Ready");
        M5_LOGI("chatbot: hello ok sid=%.16s sr=%d", h.session_id.c_str(),
                h.sample_rate);
        Serial.printf("CHAT HELLO-OK sid=%s sr=%d\r\n", h.session_id.c_str(),
                      h.sample_rate);
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
  g_ws.ev.on_binary = [](const uint8_t* data, size_t len) {
    (void)data;
    bool play = g_pipe.onBinary(len, millis());
    // PoC: 再生せずサイズのみログ。Speaking 以外は破棄則どおり捨てる。
    M5_LOGD("chatbot: bin %u bytes %s", (unsigned)len,
            play ? "(speaking)" : "(dropped)");
  };
  g_ws.ev.on_close = []() {
    M5_LOGI("chatbot: ws closed");
    g_pipe.stopSession("close", millis());
    if (!g_active || g_phase == PhIdle) return;
    if (g_phase == PhHelloWait) {
      // server hello を受ける前に切られた = hello リジェクト。
      // 連続したら自動再接続を止めて Idle に落とす (手動 retry は可)。
      g_hello_fail_count++;
      if (g_hello_fail_count >= kMaxHelloFails) {
        g_last_error = "ws hello rejected (check bind/token)";
        M5_LOGE("chatbot: %s x%d", g_last_error.c_str(), g_hello_fail_count);
        Serial.printf("CHAT WS-REJECT %s\r\n", g_last_error.c_str());
        showText("WS reject");
        setPhase(PhIdle);
        return;
      }
    }
    setPhase(PhWsConnect);
    g_last_retry_ms = millis();
  };
  g_ws.ev.on_error = [](const std::string& msg) {
    M5_LOGW("chatbot: ws error %.160s", msg.c_str());
  };
}

}  // namespace

void setupChatbot(void) {
  g_active = true;
  g_auth.begin();
  setupPipeCallbacks();
  setupWsCallbacks();
  setupMqttCallbacks();
  // expression は変えない (scaffold につき Neutral のまま)。
  showText("Chatbot...");
  // 画面ログの抑止は changeOperationMode の共通処理に委譲
  // (Avatar 稼働中は display ターゲットを ESP_LOG_NONE にする)。
  M5_LOGI("chatbot: setup ota=%s", xiaozhi::DEFAULT_OTA_URL);
  g_runtime_ws_url.clear();
  g_runtime_ws_token.clear();
  g_mqtt_cfg = xiaozhi::MqttConfig{};
  g_mqtt_sub_topic.clear();
  g_transport = TpNone;
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
      // バインド待ち: 未bindなら activate を 200 までポーリングし、
      // 成功後に OTA 取り直し (実 token 取得) してから接続する。
      if (g_need_activation) {
        if (now - g_last_activate_ms > kActivateIntervalMs) {
          g_last_activate_ms = now;
          g_activate_attempts++;
          if (doActivateOnce()) {
            g_need_activation = false;
            g_activation_code.clear();
            g_ota_attempts = 0;
            setPhase(PhOta);  // 再OTAで実 websocket token を取得
          } else if (g_activate_attempts >= kMaxActivateAttempts) {
            g_last_error = "activation timeout (bind at xiaozhi.me?)";
            M5_LOGE("chatbot: %s", g_last_error.c_str());
            setPhase(PhIdle);
          }
        }
        break;
      }
      // bind済み (websocket 同梱で code 無し) は OTA 再試行で取得を待つ。
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
        // 接続情報同梱で返った直後は接続試行
        connectWs();
      }
      break;
    case PhWsConnect:
      // now は transportLoop() 後の fresh 値 (HelloWait と同じ理由)。
      if (!transportConnected() && now - g_last_retry_ms > 5000u) {
        // 接続再試行は chatbot 側で作り直す (ライブラリ側の自動再接続は
        // ArduinoWs で 10s に抑止)。
        connectWs();
      }
      break;
    case PhHelloWait:
      // now は g_ws.loop() 後の fresh 値 (関数先頭で取得)。stale 値だと
      // sendHello() 更新時刻より小さくなり符号なしアンダーフローする。
      if (now - g_hello_sent_ms > (uint32_t)xiaozhi::kHelloTimeoutMs) {
        g_last_error = "hello timeout";
        M5_LOGE("chatbot: %s", g_last_error.c_str());
        g_pipe.onHelloFailed(now);
        g_hello_fail_count++;
        if (g_hello_fail_count >= kMaxHelloFails) {
          g_last_error = "ws hello rejected (check bind/token)";
          M5_LOGE("chatbot: %s x%d", g_last_error.c_str(), g_hello_fail_count);
          Serial.printf("CHAT WS-REJECT %s\r\n", g_last_error.c_str());
          showText("WS reject");
          setPhase(PhIdle);
          break;
        }
        transportClose();
        setPhase(PhWsConnect);
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
  if (g_phase != PhReady ||
      g_pipe.state() == xiaozhi::PipelineState::Idle ||
      g_pipe.state() == xiaozhi::PipelineState::Connecting) {
    g_last_error = "not ready (hello not done)";
    return false;
  }
  const std::string& sid = g_pipe.sessionId();
  if (sid.empty()) return false;
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
    connectWs();
    return;
  }
  // 公式は毎回 OpenAudioChannel->hello で session 更新。旧 sid 再利用はしない。
  // Idle なら再接続からやり直す。
  if (g_pipe.state() == xiaozhi::PipelineState::Idle) {
    connectWs();
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
    setPhase(PhWsConnect);
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
