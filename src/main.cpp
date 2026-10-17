#include <Avatar.h>
#include <M5Unified.h>
#include <lvgl.h>

#include "ble_gatt_cmd.h"
#include "chatbot.h"
#include "scpi_handlers.h"
#include "servo_controller.h"

// Stackchan RT
#define DXL_RX_PIN 6
#define DXL_TX_PIN 7
#define DXL_BAUD 1000000
static HardwareSerial &DXL_SERIAL = Serial1;

#define TFT_HOR_RES 320
#define TFT_VER_RES 240

enum OperationMode {
  Idle = 0,
  Menu,
  Position,
  Adjust,
  TestServo,
  BleRemote,
  Random,
  Chatbot,
  TorqueOff
};

OperationMode op_mode = OperationMode::Idle;
m5avatar::Avatar avatar;
ServoController servo_controller(DXL_SERIAL);

/* LVGL v9 menu screen */
static lv_obj_t *menu_screen = nullptr;

/* LVGL v9 servo adjust screen */
static lv_obj_t *adjust_screen = nullptr;
static lv_obj_t *adjust_x_value_label = nullptr;
static lv_obj_t *adjust_y_value_label = nullptr;

static uint32_t
    draw_buf[TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8) / 4];

static void my_disp_flush(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map) {
  uint32_t width = (area->x2 - area->x1 + 1);
  uint32_t height = (area->y2 - area->y1 + 1);
  lv_draw_sw_rgb565_swap(px_map, width * height);
  M5.Display.pushImageDMA<uint16_t>(area->x1, area->y1, width, height,
                                    (uint16_t *)px_map);
  lv_display_flush_ready(disp);
}

static void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  /* Feed M5's touch state first, like the LVGLv9 reference sample: this keeps
   * the GT911 touch controller polling fresh so LVGL sees clean press/release
   * transitions. */
  M5.update();
  lgfx::touch_point_t tp;
  if (M5.Display.getTouch(&tp)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = tp.x;
    data->point.y = tp.y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static uint32_t my_tick(void) { return millis(); }

/* The Avatar library redraws the screen from its own background task
 * (drawLoop). It can NEVER run concurrently with LVGL on the shared display
 * bus. Instead of suspend/resume (vTaskSuspend can freeze the task in the
 * middle of an SPI transaction), we fully stop the task and wait for it to
 * exit before LVGL is allowed to flush, and (re)start it only while LVGL is
 * dormant. */
static bool avatar_task_active = false;

static void avatarTaskStopAndWait() {
  if (!avatar_task_active) {
    return;
  }
  avatar.stop();
  vTaskDelay(80 /
             portTICK_PERIOD_MS); // let drawLoop finish current frame & exit
  M5.Display.waitDisplay();
  avatar_task_active = false;
}

static void avatarTaskStart() {
  if (avatar_task_active) {
    return;
  }
  M5.Display.waitDisplay();
  avatar.start();
  // Reset the expression when (re)starting the Avatar task. This is the only
  // safe place for it: Avatar::setExpression() -> suspend() ->
  // vTaskSuspend(drawTaskHandle) treats a NULL handle as "suspend the current
  // task", so it must never run while the Avatar task is stopped/never started.
  avatar.setExpression(m5avatar::Expression::Neutral);
  avatar_task_active = true;
}

/* Mode transition support. The screen ownership switches between the Avatar
 * task (LGFX direct draw) and LVGL: they can never run concurrently on the
 * shared display bus. All mode changes go through changeOperationMode() so the
 * crossing into/out of an LVGL mode is handled in one place. */
static void menu_item_cb(lv_event_t *e);

static lv_obj_t *add_menu_button(lv_obj_t *parent, const char *text,
                                 OperationMode mode) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_width(btn, 240);
  lv_obj_set_height(btn, 64);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  /* Pass the target mode as the callback user_data: lv_event_get_user_data()
   * returns the ADD-registered value (lv_event.c:133), not the object's. */
  lv_obj_add_event_cb(btn, menu_item_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)mode);
  return btn;
}

static void create_menu_screen() {
  if (menu_screen != nullptr) {
    lv_obj_delete(menu_screen);
  }
  menu_screen = lv_obj_create(NULL);

  lv_obj_t *title = lv_label_create(menu_screen);
  lv_label_set_text(title, "StackChanTesterRT Menu");
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(title, TFT_HOR_RES - 40);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *container = lv_obj_create(menu_screen);
  lv_obj_set_size(container, TFT_HOR_RES - 40, 180);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_layout(container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(container, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);

  add_menu_button(container, "BLE Remote", OperationMode::BleRemote);
  add_menu_button(container, "Idle", OperationMode::Idle);
  add_menu_button(container, "Position", OperationMode::Position);
  add_menu_button(container, "Servo Test", OperationMode::TestServo);
  add_menu_button(container, "Random", OperationMode::Random);
  add_menu_button(container, "Chatbot", OperationMode::Chatbot);
}

/* Servo offset adjust screen. One row per axis: the current offset value
 * followed by "-" / "+" buttons that re-home the servos (matching the old
 * physical-button behavior of loopAdjust()). The +/- buttons repeat while
 * held down. "Menu" returns to the menu screen. */
enum AdjustAction {
  AdjustActXMinus = 0,
  AdjustActXPlus,
  AdjustActYMinus,
  AdjustActYPlus
};

static void adjust_btn_cb(lv_event_t *e);
static void adjust_back_cb(lv_event_t *e);
static void changeOperationMode(OperationMode new_mode);

static void adjust_update_labels() {
  lv_label_set_text_fmt(adjust_x_value_label, "X:%d",
                        (int)servo_controller.getServoOffsetX());
  lv_label_set_text_fmt(adjust_y_value_label, "Y:%d",
                        (int)servo_controller.getServoOffsetY());
}

static lv_obj_t *add_adjust_step_button(lv_obj_t *parent, const char *text,
                                        AdjustAction action, lv_coord_t x,
                                        lv_coord_t y) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, 48, 40);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, adjust_btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)action);
  lv_obj_add_event_cb(btn, adjust_btn_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                      (void *)(intptr_t)action);
  return btn;
}

/* One row per axis: value label on the left, "-" / "+" on the right. */
static void create_adjust_row(lv_obj_t *parent, bool is_x, lv_coord_t y) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, y + 10);
  lv_obj_set_style_text_color(label, lv_color_black(), 0);
  if (is_x) {
    adjust_x_value_label = label;
  } else {
    adjust_y_value_label = label;
  }

  add_adjust_step_button(parent, "-", is_x ? AdjustActXMinus : AdjustActYMinus,
                         200, y);
  add_adjust_step_button(parent, "+", is_x ? AdjustActXPlus : AdjustActYPlus,
                         256, y);
}

static void create_adjust_screen() {
  if (adjust_screen != nullptr) {
    lv_obj_delete(adjust_screen);
  }
  adjust_screen = lv_obj_create(NULL);

  lv_obj_t *title = lv_label_create(adjust_screen);
  lv_label_set_text(title, "Adjust");
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_size(title, TFT_HOR_RES - 40, LV_SIZE_CONTENT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

  lv_obj_t *back = lv_button_create(adjust_screen);
  lv_obj_set_size(back, 40, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, "<-");
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back, adjust_back_cb, LV_EVENT_CLICKED, nullptr);

  create_adjust_row(adjust_screen, true, 56);
  create_adjust_row(adjust_screen, false, 120);

  adjust_update_labels();
}

static void adjust_back_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    changeOperationMode(OperationMode::Position);
  }
}

/* A mode is an "LVGL mode" if its screen is rendered by LVGL instead of the
 * Avatar task. Add new LVGL screens here. */
static bool isLVGLMode(OperationMode mode) {
  switch (mode) {
  case OperationMode::Menu:
  case OperationMode::Position:
  case OperationMode::Adjust:
    return true;
  default:
    return false;
  }
}

/* Create and load the LVGL screen for the given mode. */
static lv_obj_t *position_screen = nullptr;

static void position_back_cb(lv_event_t *e);
static void position_reset_cb(lv_event_t *e);
static void position_torqueoff_cb(lv_event_t *e);
static void position_adjust_cb(lv_event_t *e);
static void create_position_screen();
static void changeOperationMode(OperationMode new_mode);

static void create_position_screen() {
  if (position_screen != nullptr) {
    lv_obj_delete(position_screen);
  }
  position_screen = lv_obj_create(NULL);

  lv_obj_t *title = lv_label_create(position_screen);
  lv_label_set_text(title, "Position");
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(title, TFT_HOR_RES - 40);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *container = lv_obj_create(position_screen);
  lv_obj_set_size(container, TFT_HOR_RES - 40, 180);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_layout(container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(container, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *pos_label = lv_label_create(container);
  lv_label_set_text(pos_label, "Position");
  lv_obj_set_style_text_align(pos_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(pos_label, TFT_HOR_RES - 40);
  lv_obj_set_style_text_color(pos_label, lv_palette_main(LV_PALETTE_BLUE), 0);

  lv_obj_t *reset_btn = lv_button_create(container);
  lv_obj_set_size(reset_btn, 240, 64);
  lv_obj_t *reset_label = lv_label_create(reset_btn);
  lv_label_set_text(reset_label, "Reset");
  lv_obj_center(reset_label);
  lv_obj_add_event_cb(reset_btn, position_reset_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *torqueoff_btn = lv_button_create(container);
  lv_obj_set_size(torqueoff_btn, 240, 64);
  lv_obj_t *torqueoff_label = lv_label_create(torqueoff_btn);
  lv_label_set_text(torqueoff_label, "TorqueOff");
  lv_obj_center(torqueoff_label);
  lv_obj_add_event_cb(torqueoff_btn, position_torqueoff_cb, LV_EVENT_CLICKED,
                      nullptr);

  lv_obj_t *set_label = lv_label_create(container);
  lv_label_set_text(set_label, "Settings");
  lv_obj_set_style_text_align(set_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(set_label, TFT_HOR_RES - 40);
  lv_obj_set_style_text_color(set_label, lv_palette_main(LV_PALETTE_BLUE), 0);

  lv_obj_t *adjust_btn = lv_button_create(container);
  lv_obj_set_size(adjust_btn, 240, 64);
  lv_obj_t *adjust_label = lv_label_create(adjust_btn);
  lv_label_set_text(adjust_label, "Adjust");
  lv_obj_center(adjust_label);
  lv_obj_add_event_cb(adjust_btn, position_adjust_cb, LV_EVENT_CLICKED,
                      nullptr);

  lv_obj_t *back = lv_button_create(position_screen);
  lv_obj_set_size(back, 40, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, "<-");
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back, position_back_cb, LV_EVENT_CLICKED, nullptr);
}

static void position_reset_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    servo_controller.moveXY(0, 0);
    M5.delay(100);
    servo_controller.waitForAllServosToStop();
  }
}

static void position_torqueoff_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    changeOperationMode(OperationMode::TorqueOff);
  }
}

static void position_back_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    changeOperationMode(OperationMode::Menu);
  }
}

static void position_adjust_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    changeOperationMode(OperationMode::Adjust);
  }
}

static void loadLVGLScreen(OperationMode mode) {
  switch (mode) {
  case OperationMode::Menu:
    create_menu_screen();
    lv_screen_load(menu_screen);
    break;
  case OperationMode::Position:
    create_position_screen();
    lv_screen_load(position_screen);
    break;
  case OperationMode::Adjust:
    create_adjust_screen();
    lv_screen_load(adjust_screen);
    break;
  default:
    break;
  }
}

/* Virtual A/B/C touch-button area for the non-LVGL (Avatar) modes. On this
 * panel the touch buttons cover the full screen, and they are fully disabled
 * while an LVGL screen owns the display (see changeOperationMode()). */
static constexpr uint8_t TOUCH_BUTTON_RATIO = 0xFF;

/* Central mode transition: detects crossing into/out of an LVGL mode and does
 * the equivalent enterLVGL/leaveLVGL work internally.
 *  - entering an LVGL mode: fully stop the Avatar task and load the target
 *    screen, so no Avatar frame can race a subsequent LVGL flush. Also disable
 *    the virtual A/B/C touch buttons: a tap on a menu item must not double as
 *    a virtual button press, otherwise loopIdle() reports that click ~500 ms
 *    later (Button_Class hold threshold) and re-opens the menu.
 *  - leaving an LVGL mode: restore the virtual touch buttons and (re)start the
 *    Avatar task. This runs from menu_item_cb inside lv_timer_handler(), so
 *    LVGL has fully stopped rendering before the Avatar task starts drawing. */
static void changeOperationMode(OperationMode new_mode) {
  if (new_mode == op_mode) {
    return;
  }
  M5_LOGD("changeOperationMode: %d -> %d", op_mode, new_mode);

  bool lvgl_mode = isLVGLMode(op_mode);
  bool new_lvgl_mode = isLVGLMode(new_mode);

  if (op_mode == OperationMode::BleRemote) {
    teardownBLE();
  }
  if (new_mode == OperationMode::BleRemote) {
    setupBLE();
  }
  if (op_mode == OperationMode::Chatbot) {
    teardownChatbot();
  }
  if (new_mode == OperationMode::Chatbot) {
    // chatbot 側は expression を変えない (scaffold) ため、
    // Avatar タスクとの競合はなく、他モードと同様に前段で呼べる。
    setupChatbot();
  }

  if (lvgl_mode != new_lvgl_mode) {
    if (new_lvgl_mode) {
      M5.setTouchButtonHeight(0);
      avatarTaskStopAndWait();
      M5.Log.setLogLevel(m5::log_target_display, ESP_LOG_INFO);
    } else {
      M5.setTouchButtonHeightByRatio(TOUCH_BUTTON_RATIO);
      // Avatar 稼働中は画面ログを止める。M5.Log の display ターゲットは
      // 呼び出し元タスクから直接 SPI を叩くため drawLoop と競合し、
      // spiEndTransaction の mutex assert で落ちる (Chatbot で発症済み)。
      // シリアルログは継続する。
      M5.Log.setLogLevel(m5::log_target_display, ESP_LOG_NONE);
      avatarTaskStart();
    }
  }
  if (new_lvgl_mode) {
    loadLVGLScreen(new_mode);
  }

  op_mode = new_mode;
}

static void menu_item_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  const OperationMode mode = (OperationMode)(intptr_t)lv_event_get_user_data(e);
  changeOperationMode(mode);
}

static void adjust_btn_cb(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED_REPEAT) {
    return;
  }
  const auto action = (AdjustAction)(intptr_t)lv_event_get_user_data(e);
  if (action == AdjustActXMinus || action == AdjustActXPlus ||
      action == AdjustActYMinus || action == AdjustActYPlus) {
    const int delta =
        (action == AdjustActXPlus || action == AdjustActYPlus) ? 1 : -1;
    if (action == AdjustActXMinus || action == AdjustActXPlus) {
      servo_controller.setServoOffsetX(servo_controller.getServoOffsetX() +
                                       delta);
    } else {
      servo_controller.setServoOffsetY(servo_controller.getServoOffsetY() +
                                       delta);
    }
    servo_controller.moveXY(0, 0);
    adjust_update_labels();
  }
}

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
  M5.Display.setTextSize(1);
  if (M5.Display.width() < M5.Display.height()) {
    /// Landscape mode.
    M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  }
  M5.setLogDisplayIndex(0);

  M5.setTouchButtonHeightByRatio(TOUCH_BUTTON_RATIO);

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

  // NOTE: do NOT call avatar.init()/start() here. The Avatar task must stay
  // stopped while the LVGL menu owns the screen at boot.

  avatar.setBatteryIcon(true);
  avatar.setBatteryStatus(M5.Power.isCharging(), M5.Power.getBatteryLevel());

  /* LVGL v9 (mode-selection menu screen) */
  lv_init();
  lv_tick_set_cb(my_tick);
#if LV_USE_LOG != 0
  lv_log_register_print_cb([](lv_log_level_t level, const char *buf) {
    LV_UNUSED(level);
    Serial.println(buf);
    Serial.flush();
  });
#endif
  lv_display_t *disp = lv_display_create(TFT_HOR_RES, TFT_VER_RES);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  changeOperationMode(OperationMode::Menu);

  M5.Speaker.end();
}

static void loopIdle() {
  if (M5.BtnA.wasDoubleClicked()) {
    servo_controller.moveXY(0, 0);
  } else if (M5.BtnA.wasSingleClicked() || M5.BtnB.wasSingleClicked() ||
             M5.BtnC.wasSingleClicked() || M5.BtnPWR.wasClicked()) {
    /* Open the LVGL v9 menu screen to choose a mode */
    changeOperationMode(OperationMode::Menu);
  }

  // no random mouth / lyrics while idle
  avatar.setSpeechText("");
}

static void loopTestServo() {
  static uint8_t step = 0;

  if (M5.BtnPWR.wasClicked()) {
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
      changeOperationMode(OperationMode::Menu);
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
  /* Let LVGL render the servo adjust screen (touch-driven) */
  lv_timer_handler();
}

static void loopRandom() {
  static unsigned long last_action_millis = 0;

  if (M5.BtnC.wasSingleClicked()) {
    avatar.setBatteryIcon(false);
  } else if (M5.BtnA.wasReleasedAfterHold() || M5.BtnPWR.wasClicked()) {
    changeOperationMode(OperationMode::Menu);
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

static void loopChatbotMode() {
  if (M5.BtnA.wasReleasedAfterHold() || M5.BtnPWR.wasClicked()) {
    changeOperationMode(OperationMode::Menu);
    return;
  }
  if (M5.BtnA.wasSingleClicked()) {
    // 音声トグル (ToggleChatState 相当: Idle→開始 / Listening→停止 /
    // Speaking→Abort)。未接続時は再接続にフォールバックする。
    // 詳細は SCPI :CHAT:* (TEXT/START/STOP/ABORT/LISTEN)。
    chatbotToggleVoice();
  }
  ::loopChatbot();
}

static void loopTorqueOff() {
  static bool torque_off_done = false;

  if (M5.BtnPWR.wasClicked() || M5.BtnA.wasReleased() ||
      M5.BtnB.wasReleased() || M5.BtnC.wasReleased()) {
    servo_controller.torqueOn();
    torque_off_done = false;
    changeOperationMode((M5.BtnPWR.wasClicked()) ? OperationMode::Menu
                                                 : OperationMode::Position);
    return;
  }

  if (!torque_off_done) {
    servo_controller.torqueOff();
    torque_off_done = true;
    avatar.setExpression(m5avatar::Expression::Sleepy);
  }
}

static void loopBLERemote() {
  if (M5.BtnPWR.wasClicked() || M5.BtnA.wasReleased() ||
      M5.BtnB.wasReleased() || M5.BtnC.wasReleased()) {
    changeOperationMode(OperationMode::Menu);
    return;
  }
  processBLE();
}

static void loopMenu() {
  /* Let LVGL render the menu screen */
  lv_timer_handler();
}

static void loopPosition() { lv_timer_handler(); }

void loop() {
  M5.update();
  // M5_Log_async_update();

  switch (op_mode) {
  case OperationMode::Idle:
    loopIdle();
    break;
  case OperationMode::Menu:
    loopMenu();
    break;
  case OperationMode::Position:
    loopPosition();
    break;
  case OperationMode::Adjust:
    loopAdjust();
    break;
  case OperationMode::TestServo:
    loopTestServo();
    break;
  case OperationMode::Random:
    loopRandom();
    break;
  case OperationMode::Chatbot:
    loopChatbotMode();
    break;
  case OperationMode::TorqueOff:
    loopTorqueOff();
    break;
  case OperationMode::BleRemote:
    loopBLERemote();
    break;
  }
  if (op_mode != OperationMode::Menu && menu_screen != nullptr) {
    lv_obj_delete(menu_screen);
    menu_screen = nullptr;
  }

  processSCPI();
  servo_controller.update();

  M5.delay(1);
}
