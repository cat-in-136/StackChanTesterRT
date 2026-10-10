#include "chatbot.h"

#include <cstdio>
#include <string>

#include "xiaozhi/chatbot_runtime.h"

// xiaozhi chatbot 公開 facade。実処理は xiaozhi::ChatbotRuntime に委譲する。
// テキスト PoC 範囲: OTA -> MQTT -> hello -> listen検出(text) -> stt/llm/tts字幕。

namespace {
xiaozhi::ChatbotRuntime g_runtime;
char s_status_buf[160];
char s_chat_buf[192];
}  // namespace

void setupChatbot(void) { g_runtime.setup(); }

void loopChatbot(void) { g_runtime.loop(); }

void teardownChatbot(void) { g_runtime.teardown(); }

bool chatbotSendText(const char* text) { return g_runtime.sendText(text); }

void chatbotStart(void) { g_runtime.start(); }

void chatbotStop(void) { g_runtime.stop(); }

void chatbotAbort(void) { g_runtime.abort(); }

const char* chatbotStatus(void) {
  std::string s = g_runtime.status();
  snprintf(s_status_buf, sizeof(s_status_buf), "%s", s.c_str());
  return s_status_buf;
}

void chatbotSetFormatB(bool use_b) { g_runtime.setFormatB(use_b); }

bool chatbotGetFormatB(void) { return g_runtime.getFormatB(); }

const char* chatbotLastChat(void) {
  std::string s = g_runtime.lastChat();
  snprintf(s_chat_buf, sizeof(s_chat_buf), "%s", s.c_str());
  return s_chat_buf;
}
