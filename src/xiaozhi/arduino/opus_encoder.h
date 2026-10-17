#pragma once
// OpusVoiceEncoder: 上り音声用 16kHz/mono/60ms Opus エンコーダ。
// 条件は参考実装準拠 (audio_service.h AS_OPUS_ENC_CONFIG):
// complexity 0 / FEC off / DTX on / VBR on / VOIP。
// 実体は sh123/esp32_opus。
// 参照: /references/xiaozhi-esp32/main/audio/audio_service.h:60-84。
#include <cstddef>
#include <cstdint>

// opus 型の前方宣言 (実体・API prototype は cpp 側で取得)。
struct OpusEncoder;

class OpusVoiceEncoder {
 public:
  static constexpr uint32_t kSampleRate = 16000;
  static constexpr int kChannels = 1;
  static constexpr int kFrameSamples = 960;  // 60ms
  static constexpr size_t kMaxPacket = 512;  // 60ms上限に十分な余裕

  OpusVoiceEncoder();
  ~OpusVoiceEncoder();

  bool begin();
  void end();
  bool ready() const { return enc_ != nullptr; }

  // 960sample → Opus。戻り値は出力バイト数、負=失敗。
  int encode(const int16_t* pcm, uint8_t* out, size_t maxlen);

  uint32_t encodeCount() const { return encode_count_; }

 private:
  OpusEncoder* enc_ = nullptr;
  uint32_t encode_count_ = 0;
  uint32_t fail_logged_ = 0;
  uint32_t diag_logged_ = 0;
};
