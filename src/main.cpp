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
  //M5_Log_async_update();

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

void loop() {
  loop_template([](void) {
    // TODO
  });
  M5.delay(1);
}
