#include "DefaultEyes.h"

namespace {
constexpr int EYE_POS_X = -70;
constexpr int EYE_POS_Y = -16;
constexpr int EYE_OFFSET_MIN = -16;
constexpr int EYE_OFFSET_MAX = 16;
constexpr int EYE_SIZE_MIN = 8;
constexpr int EYE_SIZE_MAX = 32;
constexpr int EYE_CONTAINER_SIZE = 32;
} // namespace

DefaultEyes::DefaultEyes(lv_obj_t *parent, lv_color_t primary_color,
                         lv_color_t secondary_color, bool is_left_eye)
    : _is_left_eye(is_left_eye) {
  _container = AvatarTheme::createContainer(parent);
  lv_obj_set_size(_container, EYE_CONTAINER_SIZE, EYE_CONTAINER_SIZE);
  lv_obj_set_style_bg_opa(_container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_transform_pivot_x(_container, EYE_CONTAINER_SIZE / 2, 0);
  lv_obj_set_style_transform_pivot_y(_container, EYE_CONTAINER_SIZE / 2, 0);

  _eye = AvatarTheme::createContainer(_container);
  lv_obj_set_style_radius(_eye, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_eye, primary_color, 0);
  lv_obj_align(_eye, LV_ALIGN_CENTER, 0, 0);

  _eyelid = AvatarTheme::createContainer(_container);
  lv_obj_set_style_radius(_eyelid, 0, 0);
  lv_obj_set_style_bg_color(_eyelid, secondary_color, 0);
  lv_obj_align(_eyelid, LV_ALIGN_CENTER, 0, 0);

  setSize(0);
  setWeight(100);
  setPosition(_position);
  setRotation(0);
}

DefaultEyes::~DefaultEyes() {
  if (_container != nullptr) {
    lv_obj_delete(_container);
    _container = nullptr;
    _eye = nullptr;
    _eyelid = nullptr;
  }
}

void DefaultEyes::setPosition(const AvatarPoint &position) {
  Element::setPosition(position);
  applyPosition();
}

void DefaultEyes::applyPosition() {
  int pos_x = _is_left_eye ? EYE_POS_X : -EYE_POS_X;
  pos_x += AvatarTheme::mapRange(_position.x, -100, 100, EYE_OFFSET_MIN,
                                 EYE_OFFSET_MAX);
  int pos_y = EYE_POS_Y + AvatarTheme::mapRange(_position.y, -100, 100,
                                                EYE_OFFSET_MIN, EYE_OFFSET_MAX);
  pos_y += _breath_offset_y;

  lv_obj_align(_container, LV_ALIGN_CENTER, pos_x, pos_y);
}

void DefaultEyes::setGaze(float vertical, float horizontal) {
  if (vertical < -1.0f) {
    vertical = -1.0f;
  }
  if (vertical > 1.0f) {
    vertical = 1.0f;
  }
  if (horizontal < -1.0f) {
    horizontal = -1.0f;
  }
  if (horizontal > 1.0f) {
    horizontal = 1.0f;
  }
  _gaze_offset_x = (int)(horizontal * AvatarTheme::GAZE_RANGE_X);
  _gaze_offset_y = (int)(vertical * AvatarTheme::GAZE_RANGE_Y);
  applyGaze();
}

void DefaultEyes::applyGaze() {
  lv_obj_align(_eye, LV_ALIGN_CENTER, _gaze_offset_x, _gaze_offset_y);
  applyWeight();
}

void DefaultEyes::applyWeight() {
  _eyelid_offset_y = -AvatarTheme::mapRange(_weight, 0, 100, 0, _eye_size);
  lv_obj_align(_eyelid, LV_ALIGN_CENTER, _gaze_offset_x,
               _gaze_offset_y + _eyelid_offset_y);
}

void DefaultEyes::setBreathOffset(int offset_y) {
  if (offset_y == _breath_offset_y) {
    return;
  }
  _breath_offset_y = offset_y;
  applyPosition();
}

void DefaultEyes::setWeight(int weight) {
  Feature::setWeight(weight);
  applyWeight();
}

void DefaultEyes::setRotation(int rotation) {
  Element::setRotation(rotation);
  lv_obj_set_style_transform_rotation(_container, rotation, 0);
}

void DefaultEyes::setEmotion(Expression emotion) {
  if (getIgnoreEmotion()) {
    return;
  }

  int weight = 100;
  int rotation = 0;
  switch (emotion) {
  case Expression::Neutral:
    weight = 100;
    rotation = 0;
    break;
  case Expression::Happy:
    weight = 72;
    rotation = 1550;
    break;
  case Expression::Angry:
    weight = 70;
    rotation = 450;
    break;
  case Expression::Sad:
    weight = 70;
    rotation = -400;
    break;
  case Expression::Doubt:
    weight = 75;
    rotation = 0;
    break;
  case Expression::Sleepy:
    weight = 35;
    rotation = -50;
    break;
  }

  setWeight(weight);
  setRotation(_is_left_eye ? rotation : -rotation);
}

void DefaultEyes::setVisible(bool visible) {
  Element::setVisible(visible);
  if (visible) {
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(_container, LV_OBJ_FLAG_HIDDEN);
  }
}

void DefaultEyes::setSize(int size) {
  Feature::setSize(size);
  _eye_size = AvatarTheme::mapRange(_size, -100, 100, EYE_SIZE_MIN,
                                    EYE_SIZE_MAX);
  lv_obj_set_size(_eye, _eye_size, _eye_size);
  lv_obj_set_size(_eyelid, _eye_size, _eye_size);
  applyWeight();
}

void DefaultEyes::setColors(lv_color_t primary_color,
                            lv_color_t secondary_color) {
  lv_obj_set_style_bg_color(_eye, primary_color, 0);
  lv_obj_set_style_bg_color(_eyelid, secondary_color, 0);
}
