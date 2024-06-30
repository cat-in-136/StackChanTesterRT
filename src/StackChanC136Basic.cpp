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

enum OperationMode { Idle = 0, Random, MaxOperationMode };

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

void loop() {
  M5.update();

  if (op_mode == OperationMode::Idle) {
    if (M5.BtnC.wasPressed()) {
      op_mode = OperationMode::Random;
    }
  } else if (op_mode == OperationMode::Random) {
    loopRandom();
  }

  processSCPI();
  servo_controller.update();
  delay(1);
}
