#include "mic_capture.h"

#include <M5Unified.h>

#include <cstring>

MicCapture::MicCapture() { memset(buf_, 0, sizeof(buf_)); }

MicCapture::~MicCapture() { end(); }

bool MicCapture::start() {
  if (started_ && queued_) return true;
  ping_ = 0;
  queued_ = M5.Mic.record(buf_[ping_], kFrameSamples, kSampleRate);
  if (!queued_) {
    started_ = false;
    if (start_fail_logged_ < 5) {
      start_fail_logged_++;
      M5_LOGE("mic: record queue failed (I2S busy?)");
      Serial.println("CHAT MIC-NG record failed");
    }
    return false;
  }
  started_ = true;
  M5_LOGI("mic: capture start 16k/mono/60ms");
  Serial.println("CHAT MIC-START");
  return true;
}

void MicCapture::stop() {
  if (!started_) return;
  started_ = false;
  // 仕掛り充填は破棄 (次回 start/poll 時に上書き)。mic_task は
  // キュー空きでアイドル化するため HW は維持する (再開を速くする)。
  queued_ = false;
  M5_LOGI("mic: capture stop (frames=%lu)", (unsigned long)frame_count_);
}

void MicCapture::end() {
  stop();
  M5.Mic.end();
}

int MicCapture::micState() const {
  if (!started_) return -1;
  return (int)M5.Mic.isRecording();
}

bool MicCapture::poll(int16_t* out) {
  if (!started_ || !queued_ || !out) return false;
  if (M5.Mic.isRecording() != 0) return false;  // 充填中
  memcpy(out, buf_[ping_], sizeof(buf_[ping_]));
  frame_count_++;
  ping_ ^= 1;
  // 次バッファを即キュー (60ms周期の起点)。失敗時はセッション異常扱い。
  queued_ = M5.Mic.record(buf_[ping_], kFrameSamples, kSampleRate);
  if (!queued_) {
    M5_LOGE("mic: requeue failed at frame %lu",
            (unsigned long)frame_count_);
    started_ = false;
  }
  return true;
}
