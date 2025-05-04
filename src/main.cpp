#include <Avatar.h>
#include <M5Unified.h>
#include <functional>
#include <gob_unifiedButton.hpp>

#include "scpi_handlers.h"
#include "servo_controller.h"

goblib::UnifiedButton unifiedButton;

// Stackchan RT
#define DXL_RX_PIN 6
#define DXL_TX_PIN 7
#define DXL_BAUD 1000000
static HardwareSerial &DXL_SERIAL = Serial1;

enum OperationMode { Idle = 0, Adjust, TestSurvo, Random, MaxOperationMode };

OperationMode op_mode = OperationMode::Idle;
m5avatar::Avatar avatar;
ServoController servo_controller(DXL_SERIAL);

/**
 * @brief Task function for playing a NEC-PC-98-like PiPo sound using the
 * M5Stack speaker.
 *
 * @param param Unused parameter, set to nullptr.
 * @return void* Unused return value, set to nullptr.
 */
static void *pipoTask(void *) {
  {
    M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.tone(2000, 100);
    M5.delay(100);
    M5.Speaker.tone(1000, 100);
    M5.delay(100);
    // Do not call M5.Speaker.end() here. This cause crash..
  }
  vTaskDelete(nullptr);
  return nullptr;
}

static inline void loop_template(std::function<void(void)> func) {
  M5.update();
  unifiedButton.update();
  // M5_Log_async_update();

  func();

  processSCPI();
  servo_controller.update();
}

void setup() {
  auto cfg = M5.config();

  cfg.output_power = true;
  cfg.external_speaker.atomic_spk = true;

  M5.begin(cfg);
  Serial.begin(115200);

  M5.Log.setLogLevel(m5::log_target_display, ESP_LOG_INFO);
  // XXX Do not set log level for m5::log_target_serial! this break
  // Serial.read()!
  // M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_INFO);
  // M5.Log.setEnableColor(m5::log_target_serial, false);

  setupSCPI();

  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.setTextWrap(true);
  M5.Display.setTextSize(2);
  if (M5.Display.width() < M5.Display.height()) {
    /// Landscape mode.
    M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  }
  M5.setLogDisplayIndex(0);

  unifiedButton.begin(&M5.Display,
                      goblib::UnifiedButton::appearance_t::transparent_all);

  // M5.In_I2C.release();

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();

    if (spk_cfg.use_dac || spk_cfg.buzzer) {
      // Increasing the sample_rate will improve the sound quality instead of
      // increasing the CPU load. default:64000 (64kHz)  e.g. 48000 , 50000 ,
      // 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
      spk_cfg.sample_rate = 96000;
    }
    spk_cfg.task_pinned_core = APP_CPU_NUM;

    M5.Speaker.config(spk_cfg);
  }
  M5.Speaker.setVolume(16);
  M5.Speaker.setChannelVolume(0, 64);
  M5.Speaker.begin();
  if (M5.Speaker.isEnabled()) {
    M5_LOGI("%s", "Speaker initialized");
  } else {
    M5_LOGE("%s", "Speaker not found");
    delay(1000);
  }

  TaskHandle_t pipoTaskHandle;
  xTaskCreatePinnedToCore((TaskFunction_t)pipoTask, "pipoTask",
                          configMINIMAL_STACK_SIZE, nullptr, 2, &pipoTaskHandle,
                          xPortGetCoreID());
  if (pipoTaskHandle == NULL) { // Task creation failed
    M5_LOGE("%s", "pipoTask failed.");
  }

  DXL_SERIAL.begin(DXL_BAUD, SERIAL_8N1, DXL_RX_PIN, DXL_TX_PIN);
  delay(1000);
  servo_controller.begin();

  avatar.init();
  avatar.setBatteryIcon(true);

  M5.Speaker.end();
}

static void loopIdle() {
  if (M5.BtnA.wasDoubleClicked()) {
    servo_controller.moveXY(0, 0);
    op_mode = OperationMode::Adjust;
  } else if (M5.BtnA.wasSingleClicked()) {
    servo_controller.moveXY(0, 0);
  } else if (M5.BtnB.wasSingleClicked()) {
    op_mode = OperationMode::TestSurvo;
  } else if (M5.BtnC.wasSingleClicked()) {
    op_mode = OperationMode::Random;
  }

  // random mouth and lyrics
  if (op_mode == OperationMode::Idle) {
    static const uint16_t mouth_wait = 2000;
    static const uint16_t mouth_open_time = 200;
    static unsigned long last_mouth_millis = 0;

    static const char *lyrics[] = {"BtnA:ResetMove ", "BtnB:ServoTest  ",
                                   "BtnC:RandomMode  ", "BtnA x2:AdjustMode "};
    static const size_t lyrics_size = sizeof(lyrics) / sizeof(lyrics[0]);
    static uint_fast8_t lyrics_idx = 0;

    const unsigned long millis_since_last_mouth = millis() - last_mouth_millis;
    if (millis_since_last_mouth > mouth_open_time + mouth_wait) {
      const char *const l = lyrics[lyrics_idx++ % lyrics_size];
      avatar.setMouthOpenRatio(0.7);
      avatar.setSpeechText(l);
      last_mouth_millis = millis();
    } else if (millis_since_last_mouth > mouth_open_time) {
      avatar.setMouthOpenRatio(0.0);
    }
  } else {
    avatar.setSpeechText("");
  }
}

static void loopTestServo() {
  static uint8_t step = 0;

  if (M5.BtnB.wasPressed()) {
    avatar.setSpeechText("Stop Test Servo...");
    servo_controller.moveXY(0, 0);
    step = 100; // force quit
  }

  if (servo_controller.isMoving()) {
    // wait for moving done.
  } else {
    if (step >= 10) {
      step = 0;
      avatar.setSpeechText("");
      op_mode = OperationMode::Idle;
      return;
    } else if (step % 5 == 0) {
      avatar.setSpeechText("X   0 -> -90 ");
      servo_controller.moveX(-90);
    } else if (step % 5 == 1) {
      avatar.setSpeechText("X -90 -> +90");
      servo_controller.moveX(90);
    } else if (step % 5 == 2) {
      avatar.setSpeechText("X +90 ->   0");
      servo_controller.moveX(0);
    } else if (step % 5 == 3) {
      avatar.setSpeechText("Y   0 -> -15");
      servo_controller.moveY(-15);
    } else if (step % 5 == 4) {
      avatar.setSpeechText("Y -15 ->   0");
      servo_controller.moveY(0);
    }
    step++;
    delay(1000);
    servo_controller.waitForAllServosToStop();
  }
}

static void loopAdjust() {
  static enum adjust_mode_t {
    Init = 0,
    AdjustX,
    AdjustY
  } adjust_mode = adjust_mode_t::Init;

  if (servo_controller.isMoving()) {
    return;
  }

  bool adjust_mode_changed = false;
  int8_t delta_x = 0;
  int8_t delta_y = 0;

  if (M5.BtnA.wasPressed()) {
    if (adjust_mode == adjust_mode_t::AdjustX) {
      delta_x--;
    } else if (adjust_mode == adjust_mode_t::AdjustY) {
      delta_y--;
    }
  }
  if (M5.BtnB.wasDoubleClicked()) {
    op_mode = OperationMode::Idle;
    adjust_mode = adjust_mode_t::Init;
    avatar.setSpeechText("");
    return;
  }
  if (M5.BtnB.wasSingleClicked()) {
    if (adjust_mode == adjust_mode_t::AdjustX) {
      adjust_mode = adjust_mode_t::AdjustY;
      adjust_mode_changed = true;
    } else if (adjust_mode == adjust_mode_t::AdjustY) {
      adjust_mode = adjust_mode_t::AdjustX;
      adjust_mode_changed = true;
    }
  }
  if (M5.BtnC.wasPressed()) {
    if (adjust_mode == adjust_mode_t::AdjustX) {
      delta_x++;
    } else if (adjust_mode == adjust_mode_t::AdjustY) {
      delta_y++;
    }
  }
  if (adjust_mode == adjust_mode_t::Init) {
    adjust_mode = adjust_mode_t::AdjustX;
    adjust_mode_changed = true;
  }

  if (adjust_mode_changed || delta_x != 0 || delta_y != 0) {
    char s[64];
    if (adjust_mode == adjust_mode_t::AdjustX) {
      const auto servo_offset_x = servo_controller.getServoOffsetX() + delta_x;
      if (delta_x != 0) {
        servo_controller.setServoOffsetX(servo_offset_x);
      }
      snprintf(s, sizeof(s), "%s:%f:BtnB:X/Y", "X", servo_offset_x);
    } else if (adjust_mode == adjust_mode_t::AdjustY) {
      const auto servo_offset_y = servo_controller.getServoOffsetY() + delta_y;
      if (delta_y != 0) {
        servo_controller.setServoOffsetY(servo_offset_y);
      }
      snprintf(s, sizeof(s), "%s:%f:BtnB:X/Y", "Y", servo_offset_y);
    }
    servo_controller.moveXY(0, 0);
    avatar.setSpeechText(s);
  }
}

static void loopRandom() {
  static unsigned long last_action_millis = 0;

  if (M5.BtnC.wasDoubleClicked()) {
    avatar.setBatteryIcon(false);
  } else if (M5.BtnC.wasSingleClicked()) {
    op_mode = OperationMode::Idle;
    avatar.setBatteryIcon(true);
    return;
  }

  const auto now = millis();

  if (servo_controller.isMoving()) {
    // do nothing
  } else if (now > last_action_millis) {
    const auto x = random(-45, +45);
    const auto y = random(-10, 10);
    const auto interval_add = random(10);

    servo_controller.moveXY(x, y);
    last_action_millis = now + 2000 + 400 * interval_add;
  }

  // random mouth
  {
    static const uint16_t mouth_wait = 2000;
    static const uint16_t mouth_open_time = 200;
    static unsigned long last_mouth_millis = 0;

    const unsigned long millis_since_last_mouth = millis() - last_mouth_millis;
    if (millis_since_last_mouth > mouth_open_time + mouth_wait) {
      const float r = (max(random(15), 10l) - 10) / 5.0f;
      avatar.setMouthOpenRatio(r);
      last_mouth_millis = millis();
    } else if (millis_since_last_mouth > mouth_open_time) {
      avatar.setMouthOpenRatio(0.0);
    }
  }
}

void loop() {
  loop_template([](void) {
    if (op_mode == OperationMode::Idle) {
      loopIdle();
    } else if (op_mode == OperationMode::Adjust) {
      loopAdjust();
    } else if (op_mode == OperationMode::TestSurvo) {
      loopTestServo();
    } else if (op_mode == OperationMode::Random) {
      loopRandom();
    }
  });
  M5.delay(1);
}
