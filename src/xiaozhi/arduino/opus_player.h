#pragma once
// OpusPlayer: 復号済み生Opus (60ms/mono) → PCM → M5.Speaker.playRaw。
// デコーダは sh123/esp32_opus を直接使用する。
// AudioGeneratorOpus クラス自体は OGG コンテナ前提 (OggS demux + 48k stereo
// 固定) のため xiaozhi UDP の生フレームには使えない。opus_* API のみ借用。
// 再生は仮想ch固定。M5.Speaker.end() は呼ばない (pipoTask と同じく crash 回避)。
// mic は触らない。
#include <cstddef>
#include <cstdint>

// opus 型の前方宣言 (実体・API prototype は cpp 側で取得)。
struct OpusDecoder;

class OpusPlayer {
 public:
  static constexpr int kChannel = 1;
  // 下り最大: 24000Hz * 60ms = 1440 samples/mono。
  static constexpr int kMaxFrameSamples = 1440;

  OpusPlayer();
  ~OpusPlayer();

  // decoder @sampleRate/mono を用意。
  bool begin(int sampleRate);
  void end();
  bool ready() const { return od_ != nullptr; }
  int sampleRate() const { return sample_rate_; }

  // 1フレーム復号→再生。false=decode失敗 (欠落扱いで次へ)。
  bool play(const uint8_t* opus, size_t len);
  // 欠落時のPLC (時刻維持用。失敗しても無害)。
  void playConcealment();
  void stop();
  bool isPlaying() const;
  // Speaking 入口で呼ぶ (デコーダ状態リセット)。
  void reset();

 private:
  OpusDecoder* od_ = nullptr;
  int sample_rate_ = 24000;
  int16_t pcm_[kMaxFrameSamples];
  uint32_t play_fail_logged_ = 0;
  uint32_t diag_logged_ = 0;
};
