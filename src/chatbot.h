#pragma once
// UiShell + main 結線相当 (plan §5): Chatbot モードの orchestrator。
// テキスト PoC 範囲: OTA -> WSS -> hello -> listen検出(text) -> stt/llm/tts字幕。
// Opus 再生は対象外 (バイナリはサイズのみログ)。

void setupChatbot(void);
void loopChatbot(void);
void teardownChatbot(void);

// SCPI から呼ぶ操作。戻り値は成否。
bool chatbotSendText(const char* text);
void chatbotStart(void);   // 再接続 + listen/start
void chatbotStop(void);    // listen/stop + Idle
void chatbotAbort(void);   // abort + Close + Idle
const char* chatbotStatus(void);  // "phase=X state=Y ..." 静的バッファ
void chatbotSetFormatB(bool use_b);
bool chatbotGetFormatB(void);
const char* chatbotLastChat(void);  // "user:... / assistant:..." 静的バッファ
