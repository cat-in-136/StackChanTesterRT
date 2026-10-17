#pragma once
// UiShell + main 結線相当 (plan §5): Chatbot モードの orchestrator。
// テキスト PoC 範囲: OTA -> MQTT -> hello -> listen検出(text) -> stt/llm/tts字幕。
// 音声下り: hello応答の udp{} → UDP受信 → AES復号 → Opus再生 (M5.Speaker)。
// 音声上り: BtnA/SCPIトグル → listen/start(auto) → mic採取→Opus→UDP送信。
//   半二重 (Speaking中は採取停止)。サーバVAD任せ (端末VADなし)。

void setupChatbot(void);
void loopChatbot(void);
void teardownChatbot(void);

// SCPI から呼ぶ操作。戻り値は成否。
bool chatbotSendText(const char* text);
void chatbotStart(void);   // 再接続 + listen/start
void chatbotStop(void);    // listen/stop + Idle
void chatbotAbort(void);   // abort + Close + Idle
void chatbotToggleVoice(void);  // 音声トグル (Idle→開始/Listening→停止/Speaking→Abort)
const char* chatbotStatus(void);  // "phase=X state=Y ..." 静的バッファ
void chatbotSetFormatB(bool use_b);
bool chatbotGetFormatB(void);
const char* chatbotLastChat(void);  // "user:... / assistant:..." 静的バッファ
