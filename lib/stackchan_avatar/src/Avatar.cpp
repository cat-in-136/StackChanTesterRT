#include "Avatar.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
int ratioToWeight(float ratio) {
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
  return (int)(ratio * 100.0f + 0.5f);
}

} // namespace

Avatar::Avatar() { _palette = AvatarTheme::DefaultPalette(); }

Avatar::~Avatar() {
  if (_timer != nullptr) {
    lv_timer_delete(_timer);
    _timer = nullptr;
  }
  _speech_bubble.reset();
  _effect.reset();
  _mouth.reset();
  _right_eye.reset();
  _left_eye.reset();
  if (_screen != nullptr) {
    lv_obj_delete(_screen);
    _screen = nullptr;
    _panel = nullptr;
    _battery_obj = nullptr;
  }
}

void Avatar::begin() {
  if (_screen != nullptr) {
    return;
  }

  _screen = lv_obj_create(NULL);
  lv_obj_remove_style_all(_screen);
  lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(_screen, _palette.bg, 0);

  _panel = AvatarTheme::createContainer(_screen);
  lv_obj_set_size(_panel, AvatarTheme::SCREEN_W, AvatarTheme::SCREEN_H);
  lv_obj_align(_panel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_transform_pivot_x(_panel, AvatarTheme::SCREEN_W / 2, 0);
  lv_obj_set_style_transform_pivot_y(_panel, AvatarTheme::SCREEN_H / 2, 0);

  _left_eye.reset(
      new DefaultEyes(_panel, _palette.eye, _palette.bg, true));
  _right_eye.reset(
      new DefaultEyes(_panel, _palette.eye, _palette.bg, false));
  _mouth.reset(new DefaultMouth(_panel, _palette.mouth, _palette.bg));
  _effect.reset(
      new DefaultEffect(_panel, _palette.eye, _palette.bg));
  _speech_bubble.reset(new DefaultSpeechBubble(
      _panel, _palette.balloon_bg, _palette.balloon_text));

  // Battery icon: the whole icon is custom-drawn in one object, ported from
  // m5stack-avatar's BatteryIcon (drawRect/fillRect/fillTriangle/drawLine).
  _battery_obj = lv_obj_create(_panel);
  lv_obj_remove_style_all(_battery_obj);
  lv_obj_clear_flag(_battery_obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(_battery_obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(_battery_obj, 35, 15);
  lv_obj_align(_battery_obj, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_event_cb(_battery_obj, batteryDrawCb, LV_EVENT_DRAW_MAIN, this);
  lv_obj_add_flag(_battery_obj, LV_OBJ_FLAG_HIDDEN);

  _expression = Expression::Neutral;
  _left_eye->setEmotion(_expression);
  _right_eye->setEmotion(_expression);
  _requested_eye_weight_l = _left_eye->getWeight();
  _requested_eye_weight_r = _right_eye->getWeight();

  applyPalette();

  _next_blink_ms = lv_tick_get() + AvatarTheme::BLINK_OPEN_MS;
  _next_saccade_ms = lv_tick_get() + 1000;
  _timer = lv_timer_create(updateTimerCb, AvatarTheme::UPDATE_PERIOD_MS, this);
}

void Avatar::updateTimerCb(lv_timer_t *timer) {
  Avatar *self = (Avatar *)lv_timer_get_user_data(timer);
  if (self != nullptr) {
    self->update();
  }
}

void Avatar::applyEyeWeights(int left_weight, int right_weight) {
  _left_eye->setWeight(left_weight);
  _right_eye->setWeight(right_weight);
}

void Avatar::stopBlinkAndRestore() {
  _blinking = false;
  applyEyeWeights(_requested_eye_weight_l, _requested_eye_weight_r);
}

void Avatar::update() {
  if (_panel == nullptr) {
    return;
  }

  uint32_t now = lv_tick_get();
  float phase = (float)(now % AvatarTheme::BREATH_PERIOD_MS) /
                (float)AvatarTheme::BREATH_PERIOD_MS;
  _breath = sinf(phase * 2.0f * 3.14159265f);
  applyBreath();

  if (_transform_dirty) {
    _transform_dirty = false;
    applyPanelTransform();
  }

  if (!_auto_blink) {
    if (_blinking) {
      stopBlinkAndRestore();
    }
    return;
  }

  updateSaccade(now);

  if (_blinking) {
    if ((int32_t)(now - _blink_end_ms) >= 0) {
      stopBlinkAndRestore();
      _next_blink_ms =
          now + AvatarTheme::BLINK_OPEN_MS + lv_rand(0, 500);
    }
    return;
  }

  if ((int32_t)(now - _next_blink_ms) >= 0) {
    _blinking = true;
    _blink_end_ms = now + AvatarTheme::BLINK_CLOSED_MS;
    applyEyeWeights(AvatarTheme::BLINK_CLOSED_WEIGHT,
                    AvatarTheme::BLINK_CLOSED_WEIGHT);
  }
}

void Avatar::applyBreath() {
  if (_left_eye == nullptr) {
    return;
  }
  int eye_offset = (int)lroundf(_breath * AvatarTheme::BREATH_EYE_AMPLITUDE);
  int mouth_offset =
      (int)lroundf(_breath * AvatarTheme::BREATH_MOUTH_AMPLITUDE);
  _left_eye->setBreathOffset(eye_offset);
  _right_eye->setBreathOffset(eye_offset);
  _mouth->setBreathOffset(mouth_offset);
  _effect->setBreath(_breath);
}

void Avatar::updateSaccade(uint32_t now) {
  if ((int32_t)(now - _next_saccade_ms) < 0) {
    return;
  }

  float vertical = (float)((int)lv_rand(0, 2000) - 1000) / 1000.0f;
  float horizontal = (float)((int)lv_rand(0, 2000) - 1000) / 1000.0f;
  setGaze(vertical, horizontal);

  _next_saccade_ms =
      now + AvatarTheme::SACCADE_MIN_MS +
      lv_rand(0, AvatarTheme::SACCADE_MAX_MS - AvatarTheme::SACCADE_MIN_MS);
}

void Avatar::setSpeechText(const char *text) {
  if (text == nullptr) {
    text = "";
  }
  if (strcmp(_speech_text, text) == 0) {
    return;
  }
  strncpy(_speech_text, text, sizeof(_speech_text) - 1);
  _speech_text[sizeof(_speech_text) - 1] = 0;
  if (_speech_bubble != nullptr) {
    _speech_bubble->setSpeech(_speech_text);
  }
}

void Avatar::setMouthOpenRatio(float ratio) {
  if (_mouth != nullptr) {
    _mouth->setWeight(ratioToWeight(ratio));
  }
}

void Avatar::setExpression(Expression exp) {
  if (exp == _expression) {
    return;
  }
  _expression = exp;
  if (_left_eye == nullptr) {
    return;
  }

  _left_eye->setEmotion(exp);
  _right_eye->setEmotion(exp);
  _effect->setExpression(exp);
  _requested_eye_weight_l = _left_eye->getWeight();
  _requested_eye_weight_r = _right_eye->getWeight();

  if (_blinking) {
    _blinking = false;
    _next_blink_ms = lv_tick_get() + AvatarTheme::BLINK_OPEN_MS;
  }
}

void Avatar::setBatteryIcon(bool enabled) {
  if (enabled == _battery_icon) {
    return;
  }
  _battery_icon = enabled;
  if (_battery_obj == nullptr) {
    return;
  }
  if (enabled) {
    lv_obj_clear_flag(_battery_obj, LV_OBJ_FLAG_HIDDEN);
    updateBattery();
  } else {
    lv_obj_add_flag(_battery_obj, LV_OBJ_FLAG_HIDDEN);
  }
}

void Avatar::setBatteryStatus(bool is_charging, int level) {
  if (level < 0) {
    level = 0;
  }
  if (level > 100) {
    level = 100;
  }
  if (is_charging == _is_charging && level == _battery_level) {
    return;
  }
  _is_charging = is_charging;
  _battery_level = level;
  if (_battery_icon && _battery_obj != nullptr) {
    updateBattery();
  }
}

void Avatar::setIsAutoBlink(bool enabled) {
  if (enabled == _auto_blink) {
    return;
  }
  _auto_blink = enabled;
  if (!enabled) {
    stopBlinkAndRestore();
  } else {
    _next_blink_ms = lv_tick_get() + AvatarTheme::BLINK_OPEN_MS;
  }
}

void Avatar::setGaze(float vertical, float horizontal) {
  setLeftGaze(vertical, horizontal);
  setRightGaze(vertical, horizontal);
}

void Avatar::setLeftGaze(float vertical, float horizontal) {
  if (_left_eye == nullptr) {
    return;
  }
  _left_eye->setGaze(vertical, horizontal);
}

void Avatar::setRightGaze(float vertical, float horizontal) {
  if (_right_eye == nullptr) {
    return;
  }
  _right_eye->setGaze(vertical, horizontal);
}

void Avatar::setLeftEyeOpenRatio(float ratio) {
  int weight = ratioToWeight(ratio);
  _requested_eye_weight_l = weight;
  if (_left_eye == nullptr) {
    return;
  }
  if (_blinking) {
    stopBlinkAndRestore();
  }
  _left_eye->setWeight(weight);
}

void Avatar::setRightEyeOpenRatio(float ratio) {
  int weight = ratioToWeight(ratio);
  _requested_eye_weight_r = weight;
  if (_right_eye == nullptr) {
    return;
  }
  if (_blinking) {
    stopBlinkAndRestore();
  }
  _right_eye->setWeight(weight);
}

void Avatar::setRotation(float radian) {
  if (radian < -3.14159265f) {
    radian = -3.14159265f;
  }
  if (radian > 3.14159265f) {
    radian = 3.14159265f;
  }
  if (radian == _rotation) {
    return;
  }
  _rotation = radian;
  _transform_dirty = true;
}

void Avatar::setScale(float scale) {
  if (scale < 0.1f) {
    scale = 0.1f;
  }
  if (scale > 2.0f) {
    scale = 2.0f;
  }
  if (scale == _scale) {
    return;
  }
  _scale = scale;
  _transform_dirty = true;
}

void Avatar::applyPanelTransform() {
  if (_panel == nullptr) {
    return;
  }
  int angle = (int)(_rotation * 1800.0f / 3.14159265f);
  int zoom = (int)(_scale * 256.0f);
  lv_obj_set_style_transform_rotation(_panel, angle, 0);
  lv_obj_set_style_transform_scale_x(_panel, zoom, 0);
  lv_obj_set_style_transform_scale_y(_panel, zoom, 0);
}

void Avatar::setColorPalette(const AvatarTheme::ColorPalette &palette) {
  _palette = palette;
  applyPalette();
}

void Avatar::applyPalette() {
  if (_screen == nullptr) {
    return;
  }
  lv_obj_set_style_bg_color(_screen, _palette.bg, 0);
  lv_obj_set_style_bg_color(_panel, _palette.bg, 0);
  if (_left_eye != nullptr) {
    _left_eye->setColors(_palette.eye, _palette.bg);
    _right_eye->setColors(_palette.eye, _palette.bg);
    _mouth->setColors(_palette.mouth);
    _effect->setColors(_palette.eye, _palette.bg);
    _speech_bubble->setColors(_palette.balloon_bg, _palette.balloon_text);
  }
  if (_battery_obj != nullptr) {
    lv_obj_invalidate(_battery_obj);
  }
}

void Avatar::updateBattery() {
  if (_battery_obj != nullptr) {
    lv_obj_invalidate(_battery_obj);
  }
}

void Avatar::drawBattery(lv_layer_t *layer, lv_coord_t x, lv_coord_t y) const {
  const lv_color_t fg = _palette.battery;
  const lv_color_t bg = _palette.bg;

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.radius = 0;
  dsc.bg_opa = LV_OPA_TRANSP;
  dsc.border_opa = LV_OPA_COVER;
  dsc.border_color = fg;
  dsc.border_width = 1;

  lv_area_t area;

  // Terminal nub: drawRect(x, y + 5, 5, 5)
  area.x1 = x;
  area.y1 = y + 5;
  area.x2 = x + 4;
  area.y2 = y + 9;
  lv_draw_rect(layer, &dsc, &area);

  // Body outline: drawRect(x + 5, y, 30, 15)
  area.x1 = x + 5;
  area.y1 = y;
  area.x2 = x + 34;
  area.y2 = y + 14;
  lv_draw_rect(layer, &dsc, &area);

  // Fill: fillRect(x + 5 + 30 - battery_width, y, battery_width, 15)
  int battery_width = (int)(30.0f * ((float)_battery_level / 100.0f));
  if (battery_width > 0) {
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.radius = 0;
    fill.bg_opa = LV_OPA_COVER;
    fill.bg_color = fg;
    fill.border_width = 0;
    area.x1 = x + 5 + 30 - battery_width;
    area.y1 = y;
    area.x2 = x + 34;
    area.y2 = y + 14;
    lv_draw_rect(layer, &fill, &area);
  }

  if (_is_charging) {
    // Carve the bolt out of the fill with the background color.
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = bg;
    tri.opa = LV_OPA_COVER;

    tri.p[0] = {x + 20, y};
    tri.p[1] = {x + 15, y + 8};
    tri.p[2] = {x + 20, y + 8};
    lv_draw_triangle(layer, &tri);

    tri.p[0] = {x + 18, y + 7};
    tri.p[1] = {x + 18, y + 15};
    tri.p[2] = {x + 23, y + 7};
    lv_draw_triangle(layer, &tri);

    // Outline the bolt edges with the foreground color.
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = fg;
    line.width = 1;
    line.opa = LV_OPA_COVER;

    line.p1 = {x + 20, y};
    line.p2 = {x + 15, y + 8};
    lv_draw_line(layer, &line);
    line.p1 = {x + 20, y};
    line.p2 = {x + 20, y + 7};
    lv_draw_line(layer, &line);
    line.p1 = {x + 18, y + 15};
    line.p2 = {x + 23, y + 7};
    lv_draw_line(layer, &line);
    line.p1 = {x + 18, y + 8};
    line.p2 = {x + 18, y + 15};
    lv_draw_line(layer, &line);
  }
}

void Avatar::batteryDrawCb(lv_event_t *e) {
  Avatar *self = (Avatar *)lv_event_get_user_data(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  if (self == nullptr || obj == nullptr) {
    return;
  }
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  self->drawBattery(layer, coords.x1, coords.y1);
}
