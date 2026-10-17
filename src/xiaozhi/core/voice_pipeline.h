#pragma once
// VoicePipeline: 4状態 + stt/llm/tts dispatch + 破棄則 + 120s タイムアウト (plan §3.3)
// Clock 注入なしの軽量版: 時刻は ports 側 (millis) から tick(now_ms) で与える。
#include <cstdint>
#include <functional>
#include <string>

#include <ArduinoJson.h>
#include <cstring>

#include "types.h"

namespace xiaozhi {

struct ServerEvent {
  enum class Kind {
    None,
    Stt,           // stt{text}
    LlmEmotion,    // llm{emotion}
    TtsStart,      // tts/start
    TtsStop,       // tts/stop
    TtsSentence,   // tts/sentence_start{text}
    Alert,         // alert{...}
    Reboot,        // system{reboot}
    Goodbye,       // goodbye (サーバ切断要求)
    Ignored,       // mcp/notify/custom/glyph/system非reboot 等: 無視+ログ相当
  };
  Kind kind = Kind::None;
  std::string text;     // stt / sentence_start 本文
  std::string emotion;  // llm emotion
};

class VoicePipeline {
 public:
  struct Callbacks {
    std::function<void(PipelineState)> on_state;
    std::function<void(const std::string& role, const std::string& text)>
        on_chat_text;  // role: "user" | "assistant"
    std::function<void(const std::string& emotion)> on_emotion;
    std::function<void()> on_tts_start;
    std::function<void()> on_tts_stop;
    std::function<void(const std::string& msg)> on_alert;
    std::function<void()> on_reboot_request;
    std::function<void()> on_goodbye;  // サーバ goodbye: Idle+close は ports 側
    std::function<void()> on_timeout;  // 120s 無受信
  };

  Callbacks cb;
  // 同一 session_id で TEXT を連続送信するため Listening 維持が既定。
  // (manual でも tts/stop で Idle に落とすと 2 回目の TEXT が送れない)
  bool tts_stop_returns_to_idle = false;

  VoicePipeline() : state_(PipelineState::Idle), last_rx_ms_(0) {}

  PipelineState state() const { return state_; }
  const std::string& sessionId() const { return session_id_; }
  // tts/start 中は uplink 停止 (plan §3.3)
  bool uplinkMuted() const { return state_ == PipelineState::Speaking; }
  bool uplinkAllowed() const {
    return state_ == PipelineState::Listening && !uplinkMuted();
  }

  void startSession(uint32_t now_ms) {
    setState(PipelineState::Connecting, now_ms);
  }
  void onHelloOk(const std::string& session_id, uint32_t now_ms) {
    session_id_ = session_id;
    setState(PipelineState::Listening, now_ms);
  }
  // 音声トグル開始 (listen/start 送信成功時)。hello 直後と同様 Listening
  // に遷移させないと uplinkAllowed() が開かず上りが流れない。
  void onVoiceStart(uint32_t now_ms) { setState(PipelineState::Listening, now_ms); }
  void onHelloFailed(uint32_t now_ms) { setState(PipelineState::Idle, now_ms); }
  void stopSession(const std::string& /*reason*/, uint32_t now_ms) {
    setState(PipelineState::Idle, now_ms);
  }
  void abort(uint32_t now_ms) { setState(PipelineState::Idle, now_ms); }

  // 送信は 120s 無受信判定に影響させない (公式は受信のみ更新)。
  void noteLocalSend(uint32_t /*now_ms*/) {}

  // 受信 TEXT を dispatch。戻り値は解釈結果 (ports 側ログ用)。
  ServerEvent onText(const std::string& text, uint32_t now_ms) {
    last_rx_ms_ = now_ms;
    ServerEvent ev = classify(text);
    switch (ev.kind) {
      case ServerEvent::Kind::Stt:
        if (cb.on_chat_text) cb.on_chat_text("user", ev.text);
        break;
      case ServerEvent::Kind::LlmEmotion:
        if (cb.on_emotion) cb.on_emotion(ev.emotion);
        break;
      case ServerEvent::Kind::TtsStart:
        // 公式は Speaking 遷移が中心。既に Speaking でも再通知は許容。
        setState(PipelineState::Speaking, now_ms);
        if (cb.on_tts_start) cb.on_tts_start();
        break;
      case ServerEvent::Kind::TtsStop:
        // 公式 application.cc:629-638: Speaking 時のみ遷移、manual→Idle。
        if (state_ != PipelineState::Speaking) break;
        if (cb.on_tts_stop) cb.on_tts_stop();
        if (tts_stop_returns_to_idle)
          setState(PipelineState::Idle, now_ms);
        else
          setState(PipelineState::Listening, now_ms);
        break;
      case ServerEvent::Kind::TtsSentence:
        if (cb.on_chat_text) cb.on_chat_text("assistant", ev.text);
        break;
      case ServerEvent::Kind::Alert:
        if (cb.on_alert) cb.on_alert(ev.text);
        break;
      case ServerEvent::Kind::Reboot:
        if (cb.on_reboot_request) cb.on_reboot_request();
        break;
      case ServerEvent::Kind::Goodbye:
        // WS 層に goodbye 送信は無し (公式 WS と同じ)。上位で Idle+close。
        setState(PipelineState::Idle, now_ms);
        if (cb.on_goodbye) cb.on_goodbye();
        break;
      case ServerEvent::Kind::Ignored:
      case ServerEvent::Kind::None:
        break;
    }
    return ev;
  }

  // 受信 binary Opus。Speaking のみ再生対象、Listening 中は破棄 (plan 下り破棄則)。
  // 戻り値 true = ports 側で再生してよい (現 PoC では再生せずログのみ)。
  bool onBinary(size_t /*len*/, uint32_t now_ms) {
    last_rx_ms_ = now_ms;
    return state_ == PipelineState::Speaking;
  }

  // 120s 無受信検出 (protocol.cc:108-117 相当)。省電力制御は持ち込まない。
  // true を返したら ports 側で再接続等の処理を行うこと。
  bool checkTimeout(uint32_t now_ms) {
    if (state_ == PipelineState::Idle ||
        state_ == PipelineState::Connecting)
      return false;
    if (now_ms - last_rx_ms_ > 120000u) {
      setState(PipelineState::Idle, now_ms);
      if (cb.on_timeout) cb.on_timeout();
      return true;
    }
    return false;
  }

  static ServerEvent classify(const std::string& text) {
    ServerEvent ev;
    JsonDocument doc;
    if (deserializeJson(doc, text.c_str())) return ev;
    const char* type = doc["type"];
    if (!type) return ev;
    if (std::strcmp(type, "stt") == 0) {
      ev.kind = ServerEvent::Kind::Stt;
      const char* t = doc["text"];
      if (t) ev.text = t;
    } else if (std::strcmp(type, "llm") == 0) {
      ev.kind = ServerEvent::Kind::LlmEmotion;
      const char* e = doc["emotion"];
      if (e) ev.emotion = e;
      // text(emoji) があれば sentence 相当として使わない (emotion のみ)。
    } else if (std::strcmp(type, "tts") == 0) {
      const char* st = doc["state"];
      if (!st) return ev;
      if (std::strcmp(st, "start") == 0) {
        ev.kind = ServerEvent::Kind::TtsStart;
      } else if (std::strcmp(st, "stop") == 0) {
        ev.kind = ServerEvent::Kind::TtsStop;
      } else if (std::strcmp(st, "sentence_start") == 0) {
        ev.kind = ServerEvent::Kind::TtsSentence;
        const char* t = doc["text"];
        if (t) ev.text = t;
      } else if (std::strcmp(st, "sentence_end") == 0) {
        ev.kind = ServerEvent::Kind::Ignored;  // 文境界通知 (字幕は start 側)
      }
    } else if (std::strcmp(type, "alert") == 0) {
      ev.kind = ServerEvent::Kind::Alert;
      const char* m = doc["message"];
      if (!m) m = doc["text"];
      if (m) ev.text = m;
    } else if (std::strcmp(type, "system") == 0) {
      const char* cmd = doc["command"];
      if (cmd && std::strcmp(cmd, "reboot") == 0)
        ev.kind = ServerEvent::Kind::Reboot;
      else
        ev.kind = ServerEvent::Kind::Ignored;  // 公式は warn 無視
    } else if (std::strcmp(type, "mcp") == 0 ||
               std::strcmp(type, "notify") == 0 ||
               std::strcmp(type, "custom") == 0 ||
               std::strcmp(type, "glyph") == 0) {
      ev.kind = ServerEvent::Kind::Ignored;
    } else if (std::strcmp(type, "hello") == 0) {
      ev.kind = ServerEvent::Kind::Ignored;  // hello は WsSession 側で処理
    } else if (std::strcmp(type, "goodbye") == 0) {
      ev.kind = ServerEvent::Kind::Goodbye;
    }
    return ev;
  }

 private:
  void setState(PipelineState s, uint32_t now_ms) {
    state_ = s;
    last_rx_ms_ = now_ms;
    if (cb.on_state) cb.on_state(s);
  }

  PipelineState state_;
  std::string session_id_;
  uint32_t last_rx_ms_;
};

}  // namespace xiaozhi
