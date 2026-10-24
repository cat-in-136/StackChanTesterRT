#pragma once

#include <lvgl.h>

#include <memory>

#include "AvatarTheme.h"
#include "DefaultEffect.h"
#include "DefaultEyes.h"
#include "DefaultMouth.h"
#include "DefaultSpeechBubble.h"
#include "Expression.h"
#include "Feature.h"

class Avatar {
public:
  Avatar();
  ~Avatar();

  void begin();
  lv_obj_t *screen() const { return _screen; }

  Feature &leftEye() { return *_left_eye; }
  Feature &rightEye() { return *_right_eye; }
  Feature &mouth() { return *_mouth; }

  void setSpeechText(const char *text);
  void setMouthOpenRatio(float ratio);
  void setExpression(Expression exp);
  void setBatteryIcon(bool enabled);
  void setBatteryStatus(bool is_charging, int level);
  void setIsAutoBlink(bool enabled);
  void setGaze(float vertical, float horizontal);
  void setLeftGaze(float vertical, float horizontal);
  void setRightGaze(float vertical, float horizontal);
  void setLeftEyeOpenRatio(float ratio);
  void setRightEyeOpenRatio(float ratio);
  void setRotation(float radian);
  void setScale(float scale);

  void setColorPalette(const AvatarTheme::ColorPalette &palette);
  const AvatarTheme::ColorPalette &getColorPalette() const { return _palette; }

private:
  static void updateTimerCb(lv_timer_t *timer);

  void update();
  void applyPalette();
  void applyPanelTransform();
  void applyBreath();
  void updateSaccade(uint32_t now);
  void updateBattery();
  void drawBattery(lv_layer_t *layer, lv_coord_t x, lv_coord_t y) const;
  static void batteryDrawCb(lv_event_t *e);
  void applyEyeWeights(int left_weight, int right_weight);
  void stopBlinkAndRestore();

  lv_obj_t *_screen = nullptr;
  lv_obj_t *_panel = nullptr;
  std::unique_ptr<DefaultEyes> _left_eye;
  std::unique_ptr<DefaultEyes> _right_eye;
  std::unique_ptr<DefaultMouth> _mouth;
  std::unique_ptr<DefaultEffect> _effect;
  std::unique_ptr<DefaultSpeechBubble> _speech_bubble;
  lv_obj_t *_battery_obj = nullptr;
  lv_timer_t *_timer = nullptr;

  AvatarTheme::ColorPalette _palette;

  Expression _expression = Expression::Neutral;
  char _speech_text[192] = {0};

  bool _auto_blink = true;
  bool _blinking = false;
  uint32_t _next_blink_ms = 0;
  uint32_t _blink_end_ms = 0;
  int _requested_eye_weight_l = 100;
  int _requested_eye_weight_r = 100;

  float _breath = 0.0f;
  uint32_t _next_saccade_ms = 0;

  float _rotation = 0.0f;
  float _scale = 1.0f;
  bool _transform_dirty = false;

  bool _battery_icon = false;
  bool _is_charging = false;
  int _battery_level = 0;
};
