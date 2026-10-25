#pragma once
#include <cstdint>

// UiShell + main 結線相当 (plan §5): Chatbot モードの orchestrator。
// テキスト PoC 範囲: OTA -> MQTT -> hello -> listen検出(text) -> stt/llm/tts字幕。
// 音声下り: hello応答の udp{} → UDP受信 → AES復号 → Opus再生 (M5.Speaker)。
// 音声上り: BtnA/SCPIトグル (待機→Listening→処理待ち) → listen/start(manual)
//   → mic採取→Opus→UDP送信。tts/stop で待機へ戻る (半二重)。

void setupChatbot(void);
void loopChatbot(void);
void teardownChatbot(void);

// SCPI から呼ぶ操作。戻り値は成否。
bool chatbotSendText(const char* text);
void chatbotStart(void);   // 再接続 (必要時) + listen/start
void chatbotStop(void);    // 中断: Listening→処理待ち / 処理待ち・発話→待機 (session維持)
void chatbotAbort(void);   // session 終了 + MQTT再接続 (hello し直し)
void chatbotToggleVoice(void);  // 待機→Listening/Listening→処理待ち/処理待ち・発話→待機
const char* chatbotStatus(void);  // "phase=X state=Y ..." 静的バッファ
void chatbotSetFormatB(bool use_b);
bool chatbotGetFormatB(void);
// chatbot 音声出力 (M5.Speaker ch1) の音量 (0-255)。
void chatbotSetVolume(uint8_t volume);
uint8_t chatbotGetVolume(void);
const char* chatbotLastChat(void);  // "user:... / assistant:..." 静的バッファ
