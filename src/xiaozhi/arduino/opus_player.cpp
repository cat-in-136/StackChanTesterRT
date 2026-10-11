#include "opus_player.h"

// ESP8266Audio 同梱 libopus の API 宣言 (AudioGeneratorOpus.h が
// "libopus/include/opus.h" を相対 include している経路で解決する)。
// AudioGeneratorOpus クラス自体は使わない (OGG前提のため)。
#include <AudioGeneratorOpus.h>

#include <M5Unified.h>

#include <cstdlib>
#include <cstring>

OpusPlayer::OpusPlayer() { memset(pcm_, 0, sizeof(pcm_)); }

OpusPlayer::~OpusPlayer() { end(); }

bool OpusPlayer::begin(int sampleRate) {
  end();
  if (sampleRate <= 0) sampleRate = 24000;
  sample_rate_ = sampleRate;
  od_ = (OpusDecoder*)malloc(opus_decoder_get_size(1));
  if (!od_) {
    M5_LOGE("opus: decoder alloc failed");
    return false;
  }
  if (opus_decoder_init(od_, sample_rate_, 1) != OPUS_OK) {
    M5_LOGE("opus: decoder init failed sr=%d", sample_rate_);
    free(od_);
    od_ = nullptr;
    return false;
  }
  play_fail_logged_ = 0;
  diag_logged_ = 0;
  M5_LOGI("opus: decoder sr=%d mono", sample_rate_);
  return true;
}

void OpusPlayer::end() {
  stop();
  if (od_) {
    free(od_);
    od_ = nullptr;
  }
}

bool OpusPlayer::play(const uint8_t* opus, size_t len) {
  if (!od_ || !opus || len == 0 || len > 1500) return false;
  // 監視用: セッション先頭3フレームは decode 前後のスタック残量を出す。
  // opus_decode 1発で約4KB消費するため、loopTask 8KB既定では overflow で
  // 落ちる (Backtrace破損→再起動を確認済み)。platformio.ini で loopTask を
  // 32KB に拡大して対処。残量の後退があればここで検知できる。
  bool diag = diag_logged_ < 3;
  if (diag) diag_logged_++;
  if (diag) {
    M5_LOGD("opus: pre-decode len=%u hwm=%u", (unsigned)len,
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  }
  int samples =
      opus_decode(od_, opus, (opus_int32)len, pcm_, kMaxFrameSamples, 0);
  if (samples <= 0) {
    if (play_fail_logged_ < 5) {
      play_fail_logged_++;
      M5_LOGW("opus: decode err=%d len=%u", samples, (unsigned)len);
    }
    return false;
  }
  if (!M5.Speaker.isEnabled()) {
    M5.Speaker.begin();
  }
  if (diag) {
    M5_LOGD("opus: decoded samples=%d hwm=%u", samples,
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  }
  // mono のまま再生 (stereo アップミックス不要)。
  bool ok = M5.Speaker.playRaw(pcm_, (size_t)samples,
                               (uint32_t)sample_rate_, false, 1, kChannel);
  if (!ok && play_fail_logged_ < 10) {
    play_fail_logged_++;
    M5_LOGW("opus: playRaw busy, frame dropped");
  }
  return ok;
}

void OpusPlayer::playConcealment() {
  if (!od_) return;
  int samples = opus_decode(od_, nullptr, 0, pcm_, kMaxFrameSamples, 0);
  if (samples <= 0) return;
  if (!M5.Speaker.isEnabled()) {
    M5.Speaker.begin();
  }
  M5.Speaker.playRaw(pcm_, (size_t)samples, (uint32_t)sample_rate_, false,
                     1, kChannel);
}

void OpusPlayer::stop() {
  // 終了時はキュー破棄 (ABORT/STOP/tts-stop の即時停止用)。
  M5.Speaker.stop(kChannel);
}

bool OpusPlayer::isPlaying() const {
  return M5.Speaker.isPlaying(kChannel) > 0;
}

void OpusPlayer::reset() {
  if (od_) opus_decoder_ctl(od_, OPUS_RESET_STATE);
}
