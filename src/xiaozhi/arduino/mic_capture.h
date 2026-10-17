#pragma once
// MicCapture: M5.Mic による 16kHz/mono/60ms フレーミング採取。
// M5.Mic.record() は非ブロッキング (裏の mic_task が非同期充填) のため、
// 2面バッファを交互にキューし、充填完了 (isRecording()==0) を poll() で
// 回収する。loop 駆動で約60ms周期の PCM960 フレームが得られる。
// 半二重: Speaking 中は上位が stop() し採取停止/破棄する (端末AECなしのため)。
// 参照: /references/xiaozhi-esp32/main/audio/audio_service.h (60ms送出単位),
//       M5Unified Mic_Class::_rec_raw (非同期2スロットキュー)。
#include <cstddef>
#include <cstdint>

class MicCapture {
 public:
  static constexpr uint32_t kSampleRate = 16000;
  static constexpr int kFrameSamples = 960;  // 60ms @16kHz
  static constexpr int kFrameBytes = kFrameSamples * 2;

  MicCapture();
  ~MicCapture();

  // 採取セッション開始。false=M5.Mic.begin/record 失敗 (I2S競合等)。
  bool start();
  // 採取停止 (キュー投入を止める。mic_task はアイドル化、HWは維持)。
  // 仕掛りバッファは破棄扱い (次回 start/pol で上書き)。
  // 注意: CoreS3 は Mic/Speaker が同一 I2S ポート共有のため、Speaker
  // 再生の前には stop() ではなく end() で RX ch を破棄すること
  // (存命中は TX 再生成が IDF に拒否される)。
  void stop();
  // 完全終了 (I2S RX ch 破棄)。Speaking突入・abort/終了時用。
  // 再開は start() (再 begin)。TX 存命中でも begin は成功する。
  void end();

  bool active() const { return started_; }
  // M5.Mic.isRecording() の素通し (0=充填完了/空き, 1-2=充填中)。
  // -1=未開始。
  int micState() const;

  // 充填済み 960sample フレームを out へ複写し、次バッファをキューする。
  // 戻り値 true=1フレーム回収 (約60msに1回)。false=未充填/停止中。
  bool poll(int16_t* out);

  uint32_t frameCount() const { return frame_count_; }

 private:
  bool started_ = false;
  bool queued_ = false;
  int ping_ = 0;
  int16_t buf_[2][kFrameSamples];
  uint32_t frame_count_ = 0;
  uint32_t start_fail_logged_ = 0;
};
