#include "opus_player.h"

// sh123/esp32_opus の API 宣言 (encode+decode 対応の opus 同梱)。
// AudioGeneratorOpus クラス自体は使わない (OGG前提のため)。
#include <opus.h>

#include <M5Unified.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstdlib>
#include <cstring>

OpusPlayer::OpusPlayer() {
  for (int i = 0; i < kBufferCount; i++) {
    memset(pcm_pool_[i], 0, sizeof(pcm_pool_[i]));
  }
}

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

  // パケットキュー作成。
  pkt_queue_ = xQueueCreate(kQueueDepth, sizeof(OpusPacket));
  if (!pkt_queue_) {
    free(od_);
    od_ = nullptr;
    return false;
  }

  // プール状態リセット。
  write_idx_ = 0;
  drop_count_ = 0;
  play_fail_logged_ = 0;
  diag_logged_ = 0;

  // 再生タスク起動 (Core 0, M5Unified spk_task は Core 1)。
  task_running_ = true;
  BaseType_t ok = xTaskCreatePinnedToCore(
      &OpusPlayer::taskThunk, "opusPlay", 16384, this, 3, (TaskHandle_t*)&task_handle_, 0);
  if (ok != pdPASS) {
    task_running_ = false;
    vQueueDelete((QueueHandle_t)pkt_queue_);
    pkt_queue_ = nullptr;
    free(od_);
    od_ = nullptr;
    return false;
  }

  M5_LOGI("opus: decoder sr=%d mono (bufs=%d, q=%d)", sample_rate_,
          kBufferCount, kQueueDepth);
  return true;
}

void OpusPlayer::end() {
  stop();
  if (task_handle_) {
    task_running_ = false;
    // タスクが blocking receive なら queue へ sentinel を送って起こす。
    if (pkt_queue_) {
      OpusPacket pkt;
      memset(&pkt, 0, sizeof(pkt));
      pkt.len = 0;
      xQueueSend((QueueHandle_t)pkt_queue_, &pkt, 0);
    }
    // 終了を少し待つ (バックプレッシャ最大100ms + 余裕)。
    for (int i = 0; i < 120 && task_handle_; i++) {
      vTaskDelay(1);
    }
    if (task_handle_) {
      vTaskDelete((TaskHandle_t)task_handle_);
      task_handle_ = nullptr;
    }
  }
  if (pkt_queue_) {
    vQueueDelete((QueueHandle_t)pkt_queue_);
    pkt_queue_ = nullptr;
  }
  if (od_) {
    free(od_);
    od_ = nullptr;
  }
}

bool OpusPlayer::enqueue(int idx, int samples) {
  // バックプレッシャ: isPlaying は 0/1/2 (キュー占有率、満杯=2) を返す。
  uint32_t t0 = millis();
  while (task_running_ && M5.Speaker.isPlaying(kChannel) >= 2 &&
         (millis() - t0) < 100u) {
    vTaskDelay(1);
  }
  if (M5.Speaker.isPlaying(kChannel) >= 2) {
    drop_count_++;
    if (drop_count_ <= 10) {
      M5_LOGW("opus: queue full, frame dropped (total=%u)",
              (unsigned)drop_count_);
    }
    return false;
  }
  bool ok = M5.Speaker.playRaw(pcm_pool_[idx], (size_t)samples,
                               (uint32_t)sample_rate_, false, 1, kChannel);
  if (!ok && play_fail_logged_ < 10) {
    play_fail_logged_++;
    M5_LOGW("opus: playRaw failed");
  }
  return ok;
}

bool OpusPlayer::play(const uint8_t* opus, size_t len) {
  if (!od_ || !opus || len == 0 || len > kPacketMax) return false;
  if (!pkt_queue_) return false;

  OpusPacket pkt;
  memcpy(pkt.data, opus, len);
  pkt.len = len;

  if (xQueueSend((QueueHandle_t)pkt_queue_, &pkt, 0) != pdPASS) {
    drop_count_++;
    if (drop_count_ <= 10) {
      M5_LOGW("opus: packet queue full, frame dropped (total=%u)",
              (unsigned)drop_count_);
    }
    return false;
  }
  return true;
}

void OpusPlayer::playConcealment() {
  if (!od_ || !pkt_queue_) return;
  OpusPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.len = 0;  // センチネル → task で PLC 実行
  xQueueSend((QueueHandle_t)pkt_queue_, &pkt, 0);
}

void OpusPlayer::stop() {
  if (pkt_queue_) xQueueReset((QueueHandle_t)pkt_queue_);
  M5.Speaker.stop(kChannel);
  write_idx_ = 0;
}

bool OpusPlayer::isPlaying() const {
  return M5.Speaker.isPlaying(kChannel) > 0;
}

void OpusPlayer::reset() {
  if (od_) opus_decoder_ctl(od_, OPUS_RESET_STATE);
  if (pkt_queue_) xQueueReset((QueueHandle_t)pkt_queue_);
  write_idx_ = 0;
}

void OpusPlayer::taskThunk(void* arg) {
  static_cast<OpusPlayer*>(arg)->taskLoop();
}

void OpusPlayer::taskLoop() {
  OpusPacket pkt;
  while (task_running_) {
    if (xQueueReceive((QueueHandle_t)pkt_queue_, &pkt, portMAX_DELAY) != pdPASS) {
      continue;
    }
    if (!task_running_) break;

    int idx = write_idx_;
    int samples;
    if (pkt.len == 0) {
      // PLC (concealment)
      samples = opus_decode(od_, nullptr, 0, pcm_pool_[idx], kMaxFrameSamples, 0);
    } else {
      bool diag = diag_logged_ < 3;
      if (diag) diag_logged_++;
      if (diag) {
        M5_LOGD("opus: pre-decode len=%u", (unsigned)pkt.len);
      }
      samples = opus_decode(od_, pkt.data, (opus_int32)pkt.len, pcm_pool_[idx],
                            kMaxFrameSamples, 0);
      if (samples <= 0) {
        if (play_fail_logged_ < 5) {
          play_fail_logged_++;
          M5_LOGW("opus: decode err=%d len=%u", samples, (unsigned)pkt.len);
        }
        continue;
      }
      if (diag) {
        M5_LOGD("opus: decoded samples=%d (buf=%d)", samples, idx);
      }
    }
    if (samples <= 0) continue;

    if (!M5.Speaker.isEnabled()) {
      M5.Speaker.begin();
    }

    if (enqueue(idx, samples)) {
      write_idx_ = (write_idx_ + 1) % kBufferCount;
    }
  }
  task_handle_ = nullptr;
  vTaskDelete(nullptr);
}