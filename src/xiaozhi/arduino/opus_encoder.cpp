#include "opus_encoder.h"

// sh123/esp32_opus の API 宣言。
#include <opus.h>

#include <M5Unified.h>

#include <cstdlib>

OpusVoiceEncoder::OpusVoiceEncoder() {}

OpusVoiceEncoder::~OpusVoiceEncoder() { end(); }

bool OpusVoiceEncoder::begin() {
  end();
  int err = OPUS_OK;
  // 参考実装は ESP_OPUS_ENC_APPLICATION_AUDIO だが、用途は VOIP 相当の
  // 会話上りであり、78/esp-opus の利用例に合わせて VOIP を使う。
  enc_ = opus_encoder_create((opus_int32)kSampleRate, kChannels,
                             OPUS_APPLICATION_VOIP, &err);
  if (!enc_ || err != OPUS_OK) {
    M5_LOGE("opus-enc: create failed err=%d", err);
    enc_ = nullptr;
    return false;
  }
  // 参考実装条件 (audio_service.h AS_OPUS_ENC_CONFIG): complexity 0、
  // FEC off、DTX on、VBR on。bitrate は既定 (VBR auto)。
  opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(0));
  opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(0));
  opus_encoder_ctl(enc_, OPUS_SET_DTX(1));
  opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
  encode_count_ = 0;
  fail_logged_ = 0;
  diag_logged_ = 0;
  M5_LOGI("opus-enc: ready 16k/mono/60ms c0/fec0/dtx1/vbr1");
  return true;
}

void OpusVoiceEncoder::end() {
  if (enc_) {
    opus_encoder_destroy(enc_);
    enc_ = nullptr;
  }
}

int OpusVoiceEncoder::encode(const int16_t* pcm, uint8_t* out,
                             size_t maxlen) {
  if (!enc_ || !pcm || !out || maxlen == 0) return -1;
  bool diag = diag_logged_ < 3;
  if (diag) diag_logged_++;
  if (diag) {
    M5_LOGD("opus-enc: pre-encode hwm=%u",
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  }
  int n = opus_encode(enc_, pcm, kFrameSamples, out, (opus_int32)maxlen);
  if (n < 0) {
    if (fail_logged_ < 5) {
      fail_logged_++;
      M5_LOGW("opus-enc: encode err=%d", n);
    }
    return n;
  }
  if (diag) {
    M5_LOGD("opus-enc: %d bytes hwm=%u", n,
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  }
  encode_count_++;
  return n;
}
