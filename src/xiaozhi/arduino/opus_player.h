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
  // M5Unified playRaw はポインタ保持 (コピーなし) のため、キューに乗っている
  // 間は当該 PCM バッファを上書きできない。内部キューは最大2個なので、
  // 4面のラウンドロビン(成功時のみ進む)で必ず空きができる (doc note #29)。
  static constexpr uint8_t kBufferCount = 4;
  // ネットワーク受信と復号/再生を切り離すためのパケットキュー。
  static constexpr size_t kPacketMax = 1500;
  static constexpr int kQueueDepth = 6;

  struct OpusPacket {
    uint8_t data[kPacketMax];
    size_t len;  // 0 = PLC (concealment) センチネル
  };

  OpusPlayer();
  ~OpusPlayer();

  // decoder @sampleRate/mono を用意。パケットキューと再生タスクも起動。
  bool begin(int sampleRate);
  void end();
  bool ready() const { return od_ != nullptr; }
  int sampleRate() const { return sample_rate_; }

  // 1フレームをキューへ投入 (非ブロッキング)。false=キュー満杯/未初期化。
  bool play(const uint8_t* opus, size_t len);
  // 欠落時のPLC (キューへセンチネル投入)。
  void playConcealment();
  void stop();
  bool isPlaying() const;
  // Speaking 入口で呼ぶ (デコーダ状態リセット)。
  void reset();

  uint32_t dropCount() const { return drop_count_; }

 private:
  OpusDecoder* od_ = nullptr;
  int sample_rate_ = 24000;
  // 4面の PCM プール。
  int16_t pcm_pool_[kBufferCount][kMaxFrameSamples];
  // 次に書き込むバッファ index (enqueue 成功時のみ進む)。
  uint8_t write_idx_ = 0;

  // パケットキューと再生タスク。
  void* pkt_queue_ = nullptr;   // QueueHandle_t
  void* task_handle_ = nullptr; // TaskHandle_t
  volatile bool task_running_ = false;

  uint32_t play_fail_logged_ = 0;
  uint32_t diag_logged_ = 0;
  uint32_t drop_count_ = 0;

  // バックプレッシャ付きで pcm_pool_[idx] を再生キューへ乗せる。
  // false=キュー満杯による drop / playRaw 失敗。
  bool enqueue(int idx, int samples);

  static void taskThunk(void* arg);
  void taskLoop();
};