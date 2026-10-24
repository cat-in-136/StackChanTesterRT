#pragma once

#include <lvgl.h>

namespace AvatarTheme {

constexpr lv_coord_t SCREEN_W = 320;
constexpr lv_coord_t SCREEN_H = 240;

constexpr uint32_t UPDATE_PERIOD_MS = 33;
constexpr uint32_t BLINK_OPEN_MS = 5200;
constexpr uint32_t BLINK_CLOSED_MS = 200;
constexpr int BLINK_CLOSED_WEIGHT = 25;

constexpr int GAZE_RANGE_X = 3;
constexpr int GAZE_RANGE_Y = 3;

constexpr uint32_t BREATH_PERIOD_MS = 3300;
constexpr float BREATH_EYE_AMPLITUDE = 3.0f;
constexpr float BREATH_MOUTH_AMPLITUDE = 5.0f;

constexpr uint32_t SACCADE_MIN_MS = 500;
constexpr uint32_t SACCADE_MAX_MS = 2500;

struct ColorPalette {
  lv_color_t bg;
  lv_color_t eye;
  lv_color_t brow;
  lv_color_t mouth;
  lv_color_t balloon_bg;
  lv_color_t balloon_text;
  lv_color_t battery;
};

inline ColorPalette DefaultPalette() {
  return ColorPalette{
      lv_color_black(), lv_color_white(), lv_color_white(), lv_color_white(),
      lv_color_white(), lv_color_black(),  lv_color_white(),
  };
}

inline int mapRange(int value, int in_min, int in_max, int out_min,
                    int out_max) {
  if (in_min == in_max) {
    return out_min;
  }
  return out_min + (value - in_min) * (out_max - out_min) / (in_max - in_min);
}

inline int clampInt(int value, int lo, int hi) {
  if (value < lo) {
    return lo;
  }
  if (value > hi) {
    return hi;
  }
  return value;
}

inline lv_obj_t *createContainer(lv_obj_t *parent) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  return obj;
}

} // namespace AvatarTheme
