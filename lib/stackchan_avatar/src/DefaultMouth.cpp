#include "DefaultMouth.h"

namespace {
constexpr int MOUTH_POS_X = 0;
constexpr int MOUTH_POS_Y = 26;
constexpr int MOUTH_OFFSET_MIN = -16;
constexpr int MOUTH_OFFSET_MAX = 16;
constexpr int MOUTH_MIN_W = 90;
constexpr int MOUTH_MIN_H = 6;
constexpr int MOUTH_MAX_W = 60;
constexpr int MOUTH_MAX_H = 50;
constexpr int MOUTH_MIN_RADIUS = 0;
constexpr int MOUTH_MAX_RADIUS = 16;
} // namespace

DefaultMouth::DefaultMouth(lv_obj_t *parent, lv_color_t primary_color,
                           lv_color_t secondary_color) {
  (void)secondary_color;

  _mouth = AvatarTheme::createContainer(parent);
  lv_obj_set_style_radius(_mouth, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_mouth, primary_color, 0);

  setPosition(_position);
  setWeight(0);
  setRotation(0);
}

DefaultMouth::~DefaultMouth() {
  if (_mouth != nullptr) {
    lv_obj_delete(_mouth);
    _mouth = nullptr;
  }
}

void DefaultMouth::setPosition(const AvatarPoint &position) {
  Element::setPosition(position);
  applyPosition();
}

void DefaultMouth::applyPosition() {
  int pos_x = MOUTH_POS_X + AvatarTheme::mapRange(_position.x, -100, 100,
                                                  MOUTH_OFFSET_MIN,
                                                  MOUTH_OFFSET_MAX);
  int pos_y = MOUTH_POS_Y + AvatarTheme::mapRange(_position.y, -100, 100,
                                                  MOUTH_OFFSET_MIN,
                                                  MOUTH_OFFSET_MAX);
  pos_y += _breath_offset_y;

  lv_obj_align(_mouth, LV_ALIGN_CENTER, pos_x, pos_y);
}

void DefaultMouth::setBreathOffset(int offset_y) {
  if (offset_y == _breath_offset_y) {
    return;
  }
  _breath_offset_y = offset_y;
  applyPosition();
}

void DefaultMouth::applyWeight() {
  int size_x = AvatarTheme::mapRange(_weight, 0, 100, MOUTH_MIN_W, MOUTH_MAX_W);
  int size_y = AvatarTheme::mapRange(_weight, 0, 100, MOUTH_MIN_H, MOUTH_MAX_H);
  int radius =
      AvatarTheme::mapRange(_weight, 0, 100, MOUTH_MIN_RADIUS, MOUTH_MAX_RADIUS);

  lv_obj_set_size(_mouth, size_x, size_y);
  lv_obj_set_style_radius(_mouth, radius, 0);
  lv_obj_set_style_transform_pivot_x(_mouth, size_x / 2, 0);
  lv_obj_set_style_transform_pivot_y(_mouth, size_y / 2, 0);
}

void DefaultMouth::setWeight(int weight) {
  Feature::setWeight(weight);
  applyWeight();
}

void DefaultMouth::setRotation(int rotation) {
  Element::setRotation(rotation);
  lv_obj_set_style_transform_rotation(_mouth, rotation, 0);
}

void DefaultMouth::setVisible(bool visible) {
  Element::setVisible(visible);
  if (visible) {
    lv_obj_clear_flag(_mouth, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(_mouth, LV_OBJ_FLAG_HIDDEN);
  }
}

void DefaultMouth::setColors(lv_color_t primary_color) {
  lv_obj_set_style_bg_color(_mouth, primary_color, 0);
}
