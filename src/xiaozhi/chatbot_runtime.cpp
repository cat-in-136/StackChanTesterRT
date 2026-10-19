#include "chatbot_runtime.h"

#include <Avatar.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>

#include <ArduinoJson.h>

#include <cstring>

#include "arduino/silent_opus.h"

extern m5avatar::Avatar avatar;

namespace xiaozhi {
namespace {

// hello無応答のまま切断された連続回数。上限で自動再接続を止める。
const int kMaxHelloFails = 3;
const int kMaxOtaAttempts = 10;
const uint32_t kOtaRetryMs = 15000;
// activation ポーリング状態 (公式 ota.cc Activate / Web runActivation 互換)。
// 未bind時サーバは activation{code} を返すため、code有りの場合は
// activate が200になるまで MQTT 接続しない。
const int kMaxActivateAttempts = 20;
const uint32_t kActivateIntervalMs = 3000;

}  // namespace

const char* ChatbotRuntime::phaseStr(Phase p) {
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

void ChatbotRuntime::setPhase(Phase p) {
  phase_ = p;
  phase_since_ = millis();
}

void ChatbotRuntime::showText(const char* text) {
  // Avatar 吹き出しは ASCII 前提だが日本語も素通し (表示崩れは許容・PoC)。
  avatar.setSpeechText(text);
}

// scaffold の間は expression を変えない (Neutral のまま)。
// setExpression() は Avatar タスク停止中の呼び出しが禁止のため、
///将来有効化する場合は loop 稼働中 (Avatar task 起動後) のみ呼ぶこと。
void ChatbotRuntime::applyEmotion(const std::string& e) {
  (void)e;
}

bool ChatbotRuntime::ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  return false;
}

// WiFi 接続 (設定経路は SCPI SYST:WIFI:NET のみ)。
// /references/AirSensors_LowPower/task_wifi.cpp::connectWifi 流用:
// wifi-config が空ならエラーログ、接続失敗時もエラーログ。
void ChatbotRuntime::startWifi() {
  Preferences p;
  p.begin("wifi-config", true);
  bool has_ssid = p.isKey("WIFI_SSID");
  bool has_pass = p.isKey("WIFI_PASSWD");
  String ssid = has_ssid ? p.getString("WIFI_SSID", "") : "";
  String pass = has_pass ? p.getString("WIFI_PASSWD", "") : "";
  p.end();
  if (ssid.isEmpty() && pass.isEmpty()) {
    last_error_ = "No WiFi Setting (SYST:WIFI:NET SSID,PASS)";
    M5_LOGE("chatbot: %s", last_error_.c_str());
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

bool ChatbotRuntime::hasRuntimeServer() const {
  return !mqtt_cfg_.endpoint.empty();
}

bool ChatbotRuntime::transportConnected() { return mqtt_.isConnected(); }

void ChatbotRuntime::transportLoop() {
  mqtt_.loop();
  if (!udp_.active()) return;
  // UDP下りポーリング。再生可否は VoicePipeline の破棄則
  // (Speakingのみ再生) に従う。120s無受信の更新も onBinary が行う。
  uint32_t now = millis();
  udp_.loop(
      [this, now](const uint8_t* opus, size_t len, uint32_t ts, bool gap) {
        (void)ts;
        bool speaking = pipe_.onBinary(len, now);
        if (!speaking) return;
        if (!player_.ready()) return;
        if (gap) player_.playConcealment();
        player_.play(opus, len);
      },
      now);
}

// hello応答の udp{server,port,key,nonce} で音声下りを開く。
// mic-less のため server は上り datagram の source を下り送出先にする:
// 直後に無音フレームの punch を送り NAT マッピング+endpoint学習を行う。
void ChatbotRuntime::openUdpAudio() {
  closeUdpAudio();
  if (hello_mqtt_.udp_server.empty() || hello_mqtt_.udp_port <= 0) {
    M5_LOGI("chatbot: no udp endpoint in hello (voice off)");
    return;
  }
  if (!udp_.begin(hello_mqtt_.udp_server, hello_mqtt_.udp_port,
                  hello_mqtt_.udp_key, hello_mqtt_.udp_nonce)) {
    M5_LOGE("chatbot: udp open failed");
    return;
  }
  int sr = hello_mqtt_.sample_rate > 0 ? hello_mqtt_.sample_rate : 24000;
  if (!player_.begin(sr)) {
    M5_LOGE("chatbot: opus player init failed");
    udp_.end();
    return;
  }
  // 出力音量を反映 (player begin 後に設定)。
  M5.Speaker.setChannelVolume(OpusPlayer::kChannel, volume_);
  punchIfStale(millis(), true);
}

void ChatbotRuntime::closeUdpAudio() {
  voice_active_ = false;
  mic_.end();
  player_.stop();
  closeMouth();
  if (udp_.active()) udp_.end();
}

void ChatbotRuntime::punchIfStale(uint32_t now, bool force) {
  if (!udp_.active() || !player_.ready()) return;
  if (!force && now - last_punch_ms_ < 10000u) return;
  // 無音Opus上り (16kHz mono 60ms) を送る。目的は NAT マッピング +
  // server の下り送出先学習。空ペイロード (header のみ) では server に
  // 捨てられ UDP-RX が来ないことを実機で確認したため、必ず音声付きで送る。
  if (udp_.punch(kSilentOpus60ms, kSilentOpus60msLen, now))
    last_punch_ms_ = now;
}

void ChatbotRuntime::driveMouth(uint32_t now) {
  // Speaking+再生中のみ180msトグル (StackChan SpeakingModifier相当)。
  // setExpression は呼ばない (Avatar停止中の禁止制約)。
  bool talking = (phase_ == PhReady &&
                  pipe_.state() == PipelineState::Speaking &&
                  player_.isPlaying());
  if (talking) {
    if (now - mouth_last_ms_ > 180u) {
      mouth_last_ms_ = now;
      mouth_open_ = !mouth_open_;
      avatar.setMouthOpenRatio(mouth_open_ ? 0.7f : 0.0f);
    }
  } else if (mouth_open_) {
    closeMouth();
  }
}

void ChatbotRuntime::closeMouth() {
  if (mouth_open_) {
    mouth_open_ = false;
    avatar.setMouthOpenRatio(0.0f);
  }
}

bool ChatbotRuntime::transportSend(const std::string& text) {
  return mqtt_.sendText(text);
}

void ChatbotRuntime::transportClose() { mqtt_.close(); }

// POST {ota}activate を1回実行。200=成功, 202=待機, その他=失敗。
// 公式 ota.cc Activate / Web activate.ts 互換 (ボディは "{}" 簡易系)。
bool ChatbotRuntime::doActivateOnce() {
  DeviceIdentity id = auth_.identity();
  std::string url = activateUrlFor(DEFAULT_OTA_URL);
  HttpResult r =
      httpPost(url, buildActivateBody(activation_code_), id.device_id,
               id.client_id);
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
bool ChatbotRuntime::ensureTimeSync() {
  if (time_synced_) return true;
  const char* tz = getenv("TZ");
  if (!tz) tz = "JST-9";
  configTzTime(tz, "time.cloudflare.com", "ntp.nict.jp");
  for (int i = 0; i < 16; i++) {
    struct tm tm_info;
    if (getLocalTime(&tm_info, 500) && tm_info.tm_year > (2024 - 1900)) {
      time_synced_ = true;
      M5_LOGI("chatbot: ntp ok %04d-%02d-%02d %02d:%02d:%02d",
              tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
              tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
      return true;
    }
  }
  M5_LOGW("chatbot: ntp sync failed, TLS verify may fail");
  return false;
}

void ChatbotRuntime::doOtaOnce() {
  DeviceIdentity id = auth_.identity();
  const std::string ota = DEFAULT_OTA_URL;  // public server 固定
  std::string body = buildCheckVersionBody(id);
  M5_LOGI("chatbot: POST %s", ota.c_str());
  HttpResult r = httpPost(ota, body, id.device_id, id.client_id);
  if (r.status != 200) {
    last_error_ = "ota http " + std::to_string(r.status) + " " + r.error;
    M5_LOGE("chatbot: %s", last_error_.c_str());
    Serial.printf("CHAT OTA-NG %s\r\n", last_error_.c_str());
    return;
  }
  CheckVersionResult cv;
  if (!parseCheckVersionBody(r.body, cv)) {
    last_error_ = "ota parse failed";
    M5_LOGE("chatbot: %s body=%.200s", last_error_.c_str(), r.body.c_str());
    Serial.printf("CHAT OTA-NG %s\r\n", last_error_.c_str());
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
    need_activation_ = true;
    activation_code_ = cv.activation.code;
    activate_attempts_ = 0;
    setPhase(PhActivationWait);
    last_retry_ms_ = millis();
    return;
  }
  need_activation_ = false;
  // 制御チャネルは MQTT のみ (公式 application.cc:538-540 も mqtt 優先)。
  if (cv.mqtt.present && !cv.mqtt.endpoint.empty()) {
    mqtt_cfg_ = cv.mqtt;
    mqtt_sub_topic_ = cv.mqtt.subscribe_topic;
    if (mqtt_sub_topic_.empty() || mqtt_sub_topic_ == "null") {
      mqtt_sub_topic_ = "devices/p2p/";
      for (char c : id.device_id) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == ':') c = '_';
        mqtt_sub_topic_ += c;
      }
    }
    M5_LOGI("chatbot: ota ok, mqtt transport (endpoint=%s pub=%s sub=%s)",
            cv.mqtt.endpoint.c_str(), cv.mqtt.publish_topic.c_str(),
            mqtt_sub_topic_.c_str());
    Serial.printf("CHAT OTA-TRANSPORT mqtt sub=%s\r\n",
                  mqtt_sub_topic_.c_str());
    last_error_.clear();
    setPhase(PhMqttConnect);
    return;
  }
  // websocket は test-token 固定ダミーのため使えない。mqtt 無しは異常系。
  last_error_ = "ota: no usable transport (mqtt missing)";
  M5_LOGE("chatbot: %s", last_error_.c_str());
}

void ChatbotRuntime::connectMqtt() {
  if (mqtt_cfg_.endpoint.empty()) {
    setPhase(PhOta);
    return;
  }
  M5_LOGI("chatbot: mqtt connect %s (client %s)", mqtt_cfg_.endpoint.c_str(),
          mqtt_cfg_.client_id.c_str());
  // on_open が sendHello -> PhHelloWait へ遷移させるため、
  // 先に PhMqttConnect を立ててから begin する (上書き防止)。
  setPhase(PhMqttConnect);
  last_retry_ms_ = millis();
  if (!mqtt_.begin(mqtt_cfg_, mqtt_sub_topic_)) {
    if (phase_ == PhMqttConnect) {
      last_error_ = "mqtt connect failed";
      M5_LOGE("chatbot: %s", last_error_.c_str());
    }
    return;
  }
}

void ChatbotRuntime::sendHello() {
  std::string hello = buildMqttHello();
  pipe_.startSession(millis());
  setPhase(PhHelloWait);
  hello_sent_ms_ = millis();
  if (!transportSend(hello)) {
    last_error_ = "hello send failed (not connected)";
    M5_LOGE("chatbot: %s", last_error_.c_str());
  } else {
    M5_LOGI("chatbot: hello sent");
  }
  listen_started_ = false;
  // 新 session では旧音声は無効。RX ch も破棄 (後の Speaker 再生成のため)。
  voice_active_ = false;
  mic_.end();
}

void ChatbotRuntime::setupPipeCallbacks() {
  // 同一 session_id 継続: tts/stop 後も Listening 維持 (Idle に落とさない)。
  pipe_.tts_stop_returns_to_idle = false;
  pipe_.cb.on_state = [](PipelineState s) {
    M5_LOGI("chatbot: pipeline -> %s", toStr(s));
  };
  pipe_.cb.on_chat_text = [this](const std::string& role,
                                 const std::string& text) {
    if (role == "user") {
      last_user_ = text;
      M5_LOGI("chatbot: stt: %.120s", text.c_str());
      Serial.printf("CHAT STT %s\r\n", text.c_str());
    } else {
      last_assistant_ = text;
      showText(text.c_str());
      M5_LOGI("chatbot: tts sentence: %.120s", text.c_str());
      Serial.printf("CHAT TTS %s\r\n", text.c_str());
    }
  };
  pipe_.cb.on_emotion = [this](const std::string& e) {
    last_emotion_ = e;
    applyEmotion(e);
    Serial.printf("CHAT LLM emotion=%s\r\n", e.c_str());
  };
  pipe_.cb.on_tts_start = [this]() {
    M5_LOGI("chatbot: tts start (uplink muted)");
    Serial.println("CHAT TTS-START");
    // 半二重: Speaking 中は上り完全停止 (端末AECなしのため)。
    // stop() (キュー停止) では足りない: CoreS3 は Mic/Speaker が同一
    // I2S ポート共有で、RX ch 存命中は Speaker 側の TX 再生成が IDF に
    // 拒否される ("register I2S object to platform failed"→無音)。
    // そのため end() で RX ch 自体を破棄する。tts/stop で再開する。
    mic_.end();
    // Speaking 入口でデコーダ状態リセット (公式 ResetDecoder 相当)。
    player_.reset();
    // 下り送出先の学習し直し: 無音上りを1発送る (NAT維持)。
    punchIfStale(millis(), true);
  };
  pipe_.cb.on_tts_stop = [this]() {
    M5_LOGI("chatbot: tts stop (dropped=%u)", (unsigned)player_.dropCount());
    Serial.printf("CHAT TTS-STOP dropped=%u\\r\\n", (unsigned)player_.dropCount());
    // 再生キュー破棄 (即時停止) + 口を閉じる。
    player_.stop();
    closeMouth();
    // 次の TEXT で listen/start から送り直すため (同一 sid 継続)。
    listen_started_ = false;
    // 音声セッション継続中なら採取再開 (manual一発話では stop 済みのため
    // 通常ここは通らない。保険)。失敗時は stopVoice で閉じる。
    if (voice_active_) {
      if (!mic_.start()) {
        M5_LOGE("chatbot: mic resume failed -> voice stop");
        stopVoice();
      }
    }
  };
  pipe_.cb.on_alert = [](const std::string& m) {
    M5_LOGI("chatbot: alert %.120s", m.c_str());
    Serial.printf("CHAT ALERT %s\r\n", m.c_str());
  };
  pipe_.cb.on_goodbye = [this]() {
    // 公式: サーバ goodbye には返信しない。上位で Idle+close し再接続に任せる。
    M5_LOGI("chatbot: goodbye -> reconnect");
    Serial.println("CHAT GOODBYE");
    transportClose();
    closeUdpAudio();
    listen_started_ = false;
    if (active_) {
      setPhase(PhMqttConnect);
      last_retry_ms_ = millis();
    }
  };
  pipe_.cb.on_reboot_request = []() {
    M5_LOGI("chatbot: reboot requested -> ignored in PoC (no reboot)");
    Serial.println("CHAT REBOOT-REQ ignored");
  };
  pipe_.cb.on_timeout = [this]() {
    last_error_ = "120s rx timeout -> Idle";
    M5_LOGI("chatbot: %s", last_error_.c_str());
    transportClose();
    closeUdpAudio();
    setPhase(PhMqttConnect);
    last_retry_ms_ = millis();
  };
}

// MCP initialize / tools/list に応答する。それ以外の mcp (notifications 等) は無視。
// 戻り値: mcp メッセージだったか (hello 等への処理を継続しない)。
bool ChatbotRuntime::tryHandleMcp(const std::string& text) {
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
    std::string resp = buildMcpInitializeResult(pipe_.sessionId(), id);
    bool ok = transportSend(resp);
    M5_LOGI("chatbot: mcp initialize replied id=%d ok=%d", id, (int)ok);
  } else if (method_s == "tools/list" && has_id) {
    std::string resp = buildMcpToolsListResult(pipe_.sessionId(), id);
    bool ok = transportSend(resp);
    M5_LOGI("chatbot: mcp tools/list replied id=%d ok=%d", id, (int)ok);
  }
  return true;
}

void ChatbotRuntime::setupMqttCallbacks() {
  mqtt_.ev.on_open = [this]() {
    M5_LOGI("chatbot: mqtt open");
    sendHello();
  };
  mqtt_.ev.on_text = [this](const std::string& text) {
    // 診断: 受信メッセージを全部ログ (MCP 無視分も含めサーバ応答を可視化)
    M5_LOGI("chatbot: mqtt rx %.200s", text.c_str());
    Serial.printf("CHAT MQTT-RX %s\r\n", text.c_str());
    uint32_t now = millis();
    if (tryHandleMcp(text)) return;
    // Connecting 中は server hello として解釈
    if (pipe_.state() == PipelineState::Connecting) {
      ServerHelloMqtt h = parseServerHelloMqtt(text);
      if (h.ok) {
        hello_mqtt_ = h;
        pipe_.onHelloOk(h.session_id, now);
        hello_fail_count_ = 0;
        setPhase(PhReady);
        showText("Ready");
        M5_LOGI("chatbot: mqtt hello ok sid=%.16s udp=%s:%d",
                h.session_id.c_str(), h.udp_server.c_str(), h.udp_port);
        Serial.printf("CHAT HELLO-OK sid=%s sr=%d udp=%s:%d\r\n",
                      h.session_id.c_str(), h.sample_rate, h.udp_server.c_str(),
                      h.udp_port);
        openUdpAudio();
      } else {
        M5_LOGD("chatbot: ignore non-hello during Connecting: %.120s",
                text.c_str());
      }
      return;
    }
    ServerEvent ev = pipe_.onText(text, now);
    if (ev.kind == ServerEvent::Kind::None) {
      M5_LOGD("chatbot: ignore rx: %.160s", text.c_str());
    }
  };
  mqtt_.ev.on_close = [this]() {
    M5_LOGI("chatbot: mqtt closed");
    pipe_.stopSession("close", millis());
    closeUdpAudio();
    if (!active_ || phase_ == PhIdle) return;
    if (phase_ == PhHelloWait) {
      hello_fail_count_++;
      if (hello_fail_count_ >= kMaxHelloFails) {
        last_error_ = "mqtt hello rejected";
        M5_LOGE("chatbot: %s x%d", last_error_.c_str(), hello_fail_count_);
        Serial.printf("CHAT MQTT-REJECT %s\r\n", last_error_.c_str());
        showText("MQTT reject");
        setPhase(PhIdle);
        return;
      }
    }
    setPhase(PhMqttConnect);
    last_retry_ms_ = millis();
  };
  mqtt_.ev.on_error = [](const std::string& msg) {
    M5_LOGW("chatbot: mqtt error %.160s", msg.c_str());
  };
}

void ChatbotRuntime::setup() {
  active_ = true;
  auth_.begin();
  setupPipeCallbacks();
  setupMqttCallbacks();
  // expression は変えない (scaffold につき Neutral のまま)。
  showText("Chatbot...");
  // 画面ログの抑止は changeOperationMode の共通処理に委譲
  // (Avatar 稼働中は display ターゲットを ESP_LOG_NONE にする)。
  M5_LOGI("chatbot: setup ota=%s", DEFAULT_OTA_URL);
  mqtt_cfg_ = MqttConfig{};
  mqtt_sub_topic_.clear();
  if (!ensureWifi()) {
    startWifi();
    setPhase(PhWifi);
  } else {
    ensureTimeSync();  // 起動時に接続済みの場合も TLS 検証用に同期
    setPhase(PhOta);  // 毎起動 OTA で接続情報取得 (NVS 保存しない)
  }
  ota_attempts_ = 0;
  need_activation_ = false;
  activation_code_.clear();
  activate_attempts_ = 0;
  last_error_.clear();
  last_user_.clear();
  last_assistant_.clear();
}

void ChatbotRuntime::teardown() {
  active_ = false;
  voice_active_ = false;
  mic_.end();
  venc_.end();
  transportClose();
  closeUdpAudio();
  player_.end();
  pipe_.abort(millis());
  setPhase(PhIdle);
  WiFi.disconnect(true);
  M5_LOGI("chatbot: teardown");
}

void ChatbotRuntime::loop() {
  if (!active_) return;
  transportLoop();
  // now は on_open/on_close 等のコールバック実行後に取得する。
  // loop前の古いnowで引き算すると、コールバック内で更新された時刻
  // (hello_sent_ms_ / last_retry_ms_) より小さくなり、符号なし
  // アンダーフローで hello即timeout / 即再接続を誤発火する。
  uint32_t now = millis();
  pipe_.checkTimeout(now);
  driveMouth(now);

  switch (phase_) {
    case PhWifi:
      if (WiFi.status() == WL_CONNECTED) {
        M5_LOGI("chatbot: wifi ok %s", WiFi.localIP().toString().c_str());
        Serial.printf("CHAT WIFI-OK %s\r\n",
                      WiFi.localIP().toString().c_str());
        showText("WiFi OK");
        ensureTimeSync();  // TLS 検証用 (失敗時は警告のみで続行)
        setPhase(PhOta);
      } else if (now - phase_since_ > 20000u) {
        last_error_ = "wifi connect failed";
        M5_LOGE("chatbot: %s (SYST:WIFI:NET to set)", last_error_.c_str());
        Serial.println("CHAT WIFI-NG connect failed");
        showText("WiFi NG");
        setPhase(PhIdle);
      }
      break;
    case PhOta:
      doOtaOnce();
      ota_attempts_++;
      if (phase_ == PhOta) {  // まだ Ota ならリトライ待機へ
        if (ota_attempts_ >= kMaxOtaAttempts) {
          last_error_ = "ota max retries";
          setPhase(PhIdle);
        } else {
          setPhase(PhActivationWait);
          last_retry_ms_ = now;
        }
      }
      break;
    case PhActivationWait:
      if (need_activation_) {
        if (now - last_activate_ms_ > kActivateIntervalMs) {
          last_activate_ms_ = now;
          activate_attempts_++;
          if (doActivateOnce()) {
            need_activation_ = false;
            activation_code_.clear();
            ota_attempts_ = 0;
            setPhase(PhOta);
          } else if (activate_attempts_ >= kMaxActivateAttempts) {
            last_error_ = "activation timeout (bind at xiaozhi.me?)";
            M5_LOGE("chatbot: %s", last_error_.c_str());
            setPhase(PhIdle);
          }
        }
        break;
      }
      if (now - last_retry_ms_ > kOtaRetryMs) {
        if (ota_attempts_ >= kMaxOtaAttempts) {
          last_error_ = "activation timeout (bind at xiaozhi.me?)";
          M5_LOGE("chatbot: %s", last_error_.c_str());
          setPhase(PhIdle);
        } else {
          ota_attempts_++;
          setPhase(PhOta);
        }
      } else if (phase_ == PhActivationWait && hasRuntimeServer() &&
                 !transportConnected()) {
        connectMqtt();
      }
      break;
    case PhMqttConnect:
      if (!transportConnected() && now - last_retry_ms_ > 5000u) {
        connectMqtt();
      }
      break;
    case PhHelloWait:
      if (now - hello_sent_ms_ > (uint32_t)kHelloTimeoutMs) {
        last_error_ = "hello timeout";
        M5_LOGE("chatbot: %s", last_error_.c_str());
        pipe_.onHelloFailed(now);
        hello_fail_count_++;
        if (hello_fail_count_ >= kMaxHelloFails) {
          last_error_ = "mqtt hello rejected";
          M5_LOGE("chatbot: %s x%d", last_error_.c_str(), hello_fail_count_);
          Serial.printf("CHAT MQTT-REJECT %s\r\n", last_error_.c_str());
          showText("MQTT reject");
          setPhase(PhIdle);
          break;
        }
        transportClose();
        setPhase(PhMqttConnect);
        last_retry_ms_ = now;
      }
      break;
    case PhReady:
      // NAT維持: 下り送出先学習を保つため無音punchを定期送信。
      punchIfStale(now);
      // 音声上り (Listening+voice_active_ の間のみ。約16.7pkt/s)。
      pumpVoice(now);
      break;
    case PhIdle:
      break;
  }
}

bool ChatbotRuntime::sendText(const char* text) {
  if (!active_ || !text || !text[0]) return false;
  // 同一 session_id 継続: Idle でも Ready+接続中なら送信可とする。
  // (tts/stop 後は Listening 維持が既定だが、ABORT/STOP 直後の Idle 復帰用)。
  // hello 未完了 (Connecting) のみ拒否する。
  if (phase_ != PhReady || pipe_.state() == PipelineState::Connecting) {
    last_error_ = "not ready (hello not done)";
    return false;
  }
  if (!transportConnected()) {
    last_error_ = "not ready (transport disconnected)";
    return false;
  }
  const std::string& sid = pipe_.sessionId();
  if (sid.empty()) {
    last_error_ = "not ready (no session)";
    return false;
  }
  uint32_t now = millis();
  if (!listen_started_) {
    std::string start =
        buildListenStart(sid, "manual");  // manual 固定 (plan)
    if (!transportSend(start)) return false;
    listen_started_ = true;
  }
  // NAT維持: 下りUDPの送出先学習を保つため無音punchを適宜送る。
  punchIfStale(now);
  std::string msg = buildBypassText(sid, text, fmt_);
  bool ok = transportSend(msg);
  if (ok) {
    M5_LOGI("chatbot: tx text fmt=%s %.120s",
            fmt_ == TextFormat::A_DetectText ? "A" : "B", text);
  }
  (void)now;
  return ok;
}

void ChatbotRuntime::start() {
  if (!active_) return;
  hello_fail_count_ = 0;  // 手動リトライでは失敗カウンタをリセット
  if (!ensureWifi()) {
    startWifi();
    setPhase(PhWifi);
    return;
  }
  if (!hasRuntimeServer()) {
    ota_attempts_ = 0;
    setPhase(PhOta);  // 毎起動 OTA (保存しない)
    return;
  }
  if (!transportConnected()) {
    connectMqtt();
    return;
  }
  // 公式は毎回 OpenAudioChannel->hello で session 更新。旧 sid 再利用はしない。
  // Idle なら再接続からやり直す。
  if (pipe_.state() == PipelineState::Idle) {
    connectMqtt();
    return;
  }
}

void ChatbotRuntime::stop() {
  voice_active_ = false;
  mic_.end();
  if (pipe_.sessionId().empty()) return;
  std::string s = buildListenStop(pipe_.sessionId());
  transportSend(s);
  pipe_.stopSession("stop", millis());
  player_.stop();
  closeMouth();
  listen_started_ = false;
}

void ChatbotRuntime::abort() {
  voice_active_ = false;
  mic_.end();
  if (!pipe_.sessionId().empty()) {
    std::string s = buildAbort(pipe_.sessionId());
    transportSend(s);
  }
  pipe_.abort(millis());
  transportClose();
  closeUdpAudio();
  listen_started_ = false;
  if (active_) {
    setPhase(PhMqttConnect);
    last_retry_ms_ = millis();
  }
}

// 上りポンプ (PhReady 毎 loop から呼ぶ)。
// 採取(60ms周期)→即時 encode→即時 UDP送信。encode+send は採取周期より
// 十分速いため送信キューを持たない (公式の40pktキューは別タスク構成の
// ためのもので、滞留した古い音声を送るより直送が会話向け)。
void ChatbotRuntime::pumpVoice(uint32_t now) {
  if (!voice_active_) return;
  if (!pipe_.uplinkAllowed()) return;  // Listening の間のみ (Speaking 遮断)
  if (!udp_.active()) return;
  if (!venc_.ready() && !venc_.begin()) {
    M5_LOGE("voice: encoder init failed -> voice stop");
    Serial.println("CHAT VOICE-NG encoder");
    stopVoice();
    return;
  }
  if (!mic_.poll(voice_pcm_)) return;
  int n = venc_.encode(voice_pcm_, voice_opus_, sizeof(voice_opus_));
  if (n <= 0) return;
  if (!udp_.sendAudio(voice_opus_, (size_t)n, now)) {
    M5_LOGW("voice: udp send failed");
    return;
  }
  voice_tx_count_++;
  if (voice_tx_logged_ < 3 || (voice_tx_count_ % 100) == 0) {
    voice_tx_logged_++;
    M5_LOGI("voice: tx #%lu %dB", (unsigned long)voice_tx_count_, n);
    Serial.printf("CHAT VOICE-TX #%lu %dB\r\n", (unsigned long)voice_tx_count_,
                  n);
  }
}

bool ChatbotRuntime::startVoice() {
  if (phase_ != PhReady || pipe_.state() == PipelineState::Connecting) {
    return false;
  }
  if (!transportConnected() || pipe_.sessionId().empty()) return false;
  // I2S 半二重の切替順序 (CoreS3 レガシードライバはポート単一制約):
  // 先に Speaker タスクを落とす。この時点では TX ドライバ有効のため
  // end() は安全 (終了パスの i2s_stop が有効ドライバに当たる)。
  // 後続の Mic 開始が TX を追い出すが、タスクは既にいないため安全。
  // 逆順 (Mic 開始後の Speaker end) は無効ドライバへの i2s_stop で
  // 落ちるため禁止。TTS 側は mic_.end() 済み＋タスク停止中のため
  // begin() が新規生成する (reset/play 側では触らない)。
  M5.Speaker.end();
  // mic 先行: 失敗時は listen/start を送らず false (無音セッションを作らない)。
  if (!mic_.start()) {
    last_error_ = "mic start failed";
    M5_LOGE("chatbot: %s", last_error_.c_str());
    return false;
  }
  if (!venc_.ready() && !venc_.begin()) {
    last_error_ = "opus encoder init failed";
    M5_LOGE("chatbot: %s", last_error_.c_str());
    mic_.stop();
    return false;
  }
  // Touch to Talk (manual): 1タップ=一発話。stop で listen/stop を送り
  // 明示的に区切る (auto のサーバVAD任せは一旦使わない。録音完了が
  // 端末側で確定しないため)。
  std::string start = buildListenStart(pipe_.sessionId(), "manual");
  if (!transportSend(start)) {
    mic_.stop();
    return false;
  }
  listen_started_ = true;
  voice_active_ = true;
  pipe_.onVoiceStart(millis());
  punchIfStale(millis(), true);
  showText("Listening...");
  M5_LOGI("chatbot: voice start (manual)");
  Serial.println("CHAT LISTEN-START manual");
  return true;
}

void ChatbotRuntime::stopVoice() {
  voice_active_ = false;
  mic_.end();
  // listen/start 済みの場合のみ stop を送る (hello直後のListeningで
  // トグル停止した場合は送らない)。
  if (!pipe_.sessionId().empty() && listen_started_) {
    std::string s = buildListenStop(pipe_.sessionId());
    transportSend(s);
  }
  pipe_.stopSession("voice-stop", millis());
  listen_started_ = false;
  showText("Ready");
  M5_LOGI("chatbot: voice stop");
  Serial.println("CHAT LISTEN-STOP");
}

void ChatbotRuntime::abortVoice() {
  voice_active_ = false;
  mic_.end();
  if (!pipe_.sessionId().empty()) {
    std::string s = buildAbort(pipe_.sessionId());
    transportSend(s);
  }
  pipe_.abort(millis());
  player_.stop();
  closeMouth();
  listen_started_ = false;
  showText("Ready");
  M5_LOGI("chatbot: voice abort");
  Serial.println("CHAT LISTEN-ABORT");
}

// ToggleChatState 主系のうちウェイクワードを除いた部分
// (application.cc HandleToggleChatState 互換)。
void ChatbotRuntime::toggleVoice() {
  if (!active_) return;
  PipelineState s = pipe_.state();
  if (s == PipelineState::Listening) {
    stopVoice();
    return;
  }
  if (s == PipelineState::Speaking) {
    abortVoice();
    return;
  }
  // Idle/Connecting: 未接続なら再接続、既接続なら開始。
  if (phase_ != PhReady || !transportConnected() ||
      pipe_.sessionId().empty()) {
    start();
    return;
  }
  if (!startVoice()) {
    last_error_ = "voice start failed (not ready?)";
    M5_LOGE("chatbot: %s", last_error_.c_str());
    Serial.printf("CHAT LISTEN-NG %s\r\n", last_error_.c_str());
  }
}

void ChatbotRuntime::setFormatB(bool use_b) {
  fmt_ =
      use_b ? TextFormat::B_TypeText : TextFormat::A_DetectText;
}

void ChatbotRuntime::setVolume(uint8_t volume) {
  volume_ = volume;
  // 即時反映 (再生中でも次フレームから適用)。
  M5.Speaker.setChannelVolume(OpusPlayer::kChannel, volume_);
  // 変更確認音 (Speaking/voice中は鳴らさない)。
  if (!voice_active_ && pipe_.state() != PipelineState::Speaking) {
    M5.Speaker.tone(1000, 80, OpusPlayer::kChannel);
  }
}

bool ChatbotRuntime::getFormatB() const {
  return fmt_ == TextFormat::B_TypeText;
}

std::string ChatbotRuntime::status() const {
  char buf[160];
  snprintf(buf, sizeof(buf),
            "phase=%s pipe=%s wifi=%d fmt=%s udp=%d rx=%lu tx=%lu voice=%d "
            "mic=%d vtx=%lu vol=%u",
            phaseStr(phase_), toStr(pipe_.state()), (int)WiFi.status(),
            fmt_ == TextFormat::A_DetectText ? "A" : "B",
            udp_.active() ? 1 : 0, (unsigned long)udp_.rxCount(),
            (unsigned long)udp_.txCount(), voice_active_ ? 1 : 0,
            mic_.micState(), (unsigned long)voice_tx_count_, volume_);
  return std::string(buf);
}

std::string ChatbotRuntime::lastChat() const {
  char buf[192];
  snprintf(buf, sizeof(buf), "user=%.60s asm=%.60s emo=%.20s err=%.40s",
           last_user_.c_str(), last_assistant_.c_str(),
           last_emotion_.c_str(), last_error_.c_str());
  return std::string(buf);
}

}  // namespace xiaozhi
