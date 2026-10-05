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
xiaozhi::VoicePipeline g_pipe;
xiaozhi::TextFormat g_fmt = xiaozhi::TextFormat::A_DetectText;
xiaozhi::ServerHello g_hello;

bool g_active = false;
Phase g_phase = PhIdle;
uint32_t g_phase_since = 0;
uint32_t g_hello_sent_ms = 0;
uint32_t g_last_retry_ms = 0;
int g_ota_attempts = 0;
const int kMaxOtaAttempts = 10;
const uint32_t kOtaRetryMs = 15000;

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

// websocket 接続情報はランタイム保持のみ (NVS 保存しない)。
// 毎起動 OTA で取り直す。OTA URL は public server 固定。
static std::string g_runtime_ws_url;
static std::string g_runtime_ws_token;

bool hasRuntimeServer() { return !g_runtime_ws_url.empty(); }

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
  if (!cv.activation.code.empty()) {
    // 初回 (token 無し) は xiaozhi.me 用の 6桁コードを画面表示する。
    char buf[64];
    snprintf(buf, sizeof(buf), "Code:%s", cv.activation.code.c_str());
    showText(buf);
    M5_LOGI("chatbot: activation %s %s", buf,
            cv.activation.message.c_str());
    Serial.printf("CHAT ACTIVATION code=%s msg=%s\r\n",
                  cv.activation.code.c_str(), cv.activation.message.c_str());
    if (!cv.has_websocket) {
      // 未バインド: websocket 無し。バインド待ちリトライへ。
      setPhase(PhActivationWait);
      g_last_retry_ms = millis();
      return;
    }
  }
  if (!cv.has_websocket) {
    g_last_error = "ota: no websocket and no code";
    M5_LOGE("chatbot: %s", g_last_error.c_str());
    return;
  }
  g_runtime_ws_url = cv.server.url;
  g_runtime_ws_token = cv.server.token;
  M5_LOGI("chatbot: ota ok, wss url cached (runtime)");
  g_last_error.clear();
  setPhase(PhWsConnect);
}

void connectWs() {
  if (g_runtime_ws_url.empty()) {
    setPhase(PhOta);
    return;
  }
  xiaozhi::DeviceIdentity id = g_auth.identity();
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
  std::string hello = xiaozhi::buildHello();
  g_pipe.startSession(millis());
  setPhase(PhHelloWait);
  g_hello_sent_ms = millis();
  if (!g_ws.sendText(hello)) {
    g_last_error = "hello send failed (not connected)";
    M5_LOGE("chatbot: %s", g_last_error.c_str());
  } else {
    M5_LOGI("chatbot: hello sent");
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
    // 公式: WS に goodbye 返信なし。上位で Idle+close し再接続に任せる。
    M5_LOGI("chatbot: goodbye -> reconnect");
    Serial.println("CHAT GOODBYE");
    g_ws.close();
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
    g_ws.close();
    setPhase(PhWsConnect);
    g_last_retry_ms = millis();
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
    if (g_active && g_phase != PhIdle) {
      setPhase(PhWsConnect);
      g_last_retry_ms = millis();
    }
  };
}

}  // namespace

void setupChatbot(void) {
  g_active = true;
  g_auth.begin();
  setupPipeCallbacks();
  setupWsCallbacks();
  // expression は変えない (scaffold につき Neutral のまま)。
  showText("Chatbot...");
  // 画面ログの抑止は changeOperationMode の共通処理に委譲
  // (Avatar 稼働中は display ターゲットを ESP_LOG_NONE にする)。
  M5_LOGI("chatbot: setup ota=%s", xiaozhi::DEFAULT_OTA_URL);
  g_runtime_ws_url.clear();
  g_runtime_ws_token.clear();
  if (!ensureWifi()) {
    startWifi();
    setPhase(PhWifi);
  } else {
    ensureTimeSync();  // 起動時に接続済みの場合も TLS 検証用に同期
    setPhase(PhOta);  // 毎起動 OTA で websocket 取得 (NVS 保存しない)
  }
  g_ota_attempts = 0;
  g_last_error.clear();
  g_last_user.clear();
  g_last_assistant.clear();
}

void teardownChatbot(void) {
  g_active = false;
  g_ws.close();
  g_pipe.abort(millis());
  setPhase(PhIdle);
  WiFi.disconnect(true);
  M5_LOGI("chatbot: teardown");
}

void loopChatbot(void) {
  if (!g_active) return;
  uint32_t now = millis();
  g_ws.loop();
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
      // バインド待ち: OTA 再試行で websocket 取得を待つ。
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
                 !g_ws.isConnected()) {
        // websocket 同梱で返った直後は接続試行
        connectWs();
      }
      break;
    case PhWsConnect:
      if (!g_ws.isConnected() && now - g_last_retry_ms > 5000u) {
        // 非同期接続の再試行は chatbot 側で作り直す (ライブラリ側の
        // 自動再接続は ArduinoWs で 10s に抑止)。
        connectWs();
      }
      break;
    case PhHelloWait:
      if (now - g_hello_sent_ms > (uint32_t)xiaozhi::kHelloTimeoutMs) {
        g_last_error = "hello timeout";
        M5_LOGE("chatbot: %s", g_last_error.c_str());
        g_pipe.onHelloFailed(now);
        g_ws.close();
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
    if (!g_ws.sendText(start)) return false;
    g_listen_started = true;
  }
  std::string msg = xiaozhi::buildBypassText(sid, text, g_fmt);
  bool ok = g_ws.sendText(msg);
  if (ok) {
    M5_LOGI("chatbot: tx text fmt=%s %.120s",
            g_fmt == xiaozhi::TextFormat::A_DetectText ? "A" : "B", text);
  }
  (void)now;
  return ok;
}

void chatbotStart(void) {
  if (!g_active) return;
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
  if (!g_ws.isConnected()) {
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
  g_ws.sendText(s);
  g_pipe.stopSession("stop", millis());
  g_listen_started = false;
}

void chatbotAbort(void) {
  if (!g_pipe.sessionId().empty()) {
    std::string s = xiaozhi::buildAbort(g_pipe.sessionId());
    g_ws.sendText(s);
  }
  g_pipe.abort(millis());
  g_ws.close();
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
