#include <M5Unified.h>
#if __has_include(<M5StackUpdater.h>)
#include <M5StackUpdater.h>
#endif

#include "scpi_handlers.h"
#include "servo_controller.h"
#include <Avatar.h>

// M5Stack Basic/Gray/Go
// Port.A X:G22, Y:G21
// Port.C X:G16, Y:G17
// Stack-chan PCB: X:G5, Y:G2
// hand-maid: X:G2, Y:G5
#define SERVO_PIN_X 2
#define SERVO_PIN_Y 5

#define SDU_APP_PATH "/StackChanC136.bin"
#define TFCARD_CS_PIN 4

enum OperationMode { Idle = 0, TestSurvo, Random, MaxOperationMode };

OperationMode op_mode = OperationMode::Idle;
m5avatar::Avatar avatar;
ServoController servo_controller;

void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;
  M5.begin(cfg);

#if __has_include(<M5StackUpdater.h>)
  M5.update();
  if (M5.BtnA.isPressed()) {
    M5_LOGI("Will Load menu binary");
    updateFromFS(SD);
    ESP.restart();
  }
#endif // if __has_include(<M5StackUpdater.h>)

  M5.Log.setLogLevel(m5::log_target_display, ESP_LOG_NONE);
  M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_INFO);
  // M5.Log.setEnableColor(m5::log_target_serial, false);

  M5.In_I2C.release();

  setupSCPI();
  servo_controller.begin(SERVO_PIN_X, SERVO_PIN_Y);

  avatar.init();
  avatar.setBatteryIcon(true);
}

static void loopTestServo() {
  static uint8_t step = 0;

  if (M5.BtnB.wasSingleClicked()) {
    avatar.setSpeechText("Stop Test Servo...");
    servo_controller.moveXY(90, 90, 1000);
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
      avatar.setSpeechText("X 90 -> 0  ");
      servo_controller.moveX(0);
    } else if (step % 5 == 1) {
      avatar.setSpeechText("X 0 -> 180  ");
      servo_controller.moveX(180);
    } else if (step % 5 == 2) {
      avatar.setSpeechText("X 180 -> 90  ");
      servo_controller.moveX(90);
    } else if (step % 5 == 3) {
      avatar.setSpeechText("Y 90 -> 50  ");
      servo_controller.moveY(50);
    } else if (step % 5 == 4) {
      avatar.setSpeechText("Y 50 -> 90  ");
      servo_controller.moveY(90);
    }
    step++;
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
    const auto x = random(45, 135);
    const auto y = random(60, 80);
    const auto delay_time = random(10);
    const auto interval_add = random(10);

    servo_controller.moveXY(x, y, 1000 + 100 * delay_time);
    last_action_millis =
        now + 1000 + 100 * delay_time + 1000 + 400 * interval_add;
  }
}

static void loopRandomMouthOpen() {
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

void loop() {
  M5.update();

  if (op_mode == OperationMode::Idle) {
    if (M5.BtnB.wasSingleClicked()) {
      op_mode = OperationMode::TestSurvo;
    } else if (M5.BtnC.wasPressed()) {
      op_mode = OperationMode::Random;
    }
    loopRandomMouthOpen();
  } else if (op_mode == OperationMode::TestSurvo) {
    loopTestServo();
  } else if (op_mode == OperationMode::Random) {
    loopRandom();
    loopRandomMouthOpen();
  }

  processSCPI();
  servo_controller.update();
  delay(1);
}
