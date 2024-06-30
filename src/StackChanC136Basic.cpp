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

void loop() {
  M5.update();

  processSCPI();
  servo_controller.update();
  delay(1);
}
