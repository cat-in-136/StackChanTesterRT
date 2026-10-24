#pragma once

#include "Feature.h"

class DefaultEyes : public Feature {
public:
  DefaultEyes(lv_obj_t *parent, lv_color_t primary_color,
              lv_color_t secondary_color, bool is_left_eye);
  ~DefaultEyes() override;

  void setPosition(const AvatarPoint &position) override;
  void setWeight(int weight) override;
  void setRotation(int rotation) override;
  void setEmotion(Expression emotion) override;
  void setVisible(bool visible) override;
  void setSize(int size) override;
  void setGaze(float vertical, float horizontal) override;
  void setBreathOffset(int offset_y) override;

  void setColors(lv_color_t primary_color, lv_color_t secondary_color);

  lv_obj_t *getContainer() const { return _container; }

private:
  void applyPosition();
  void applyGaze();
  void applyWeight();

  bool _is_left_eye = false;
  int _eye_size = 20;
  int _eyelid_offset_y = 0;
  int _gaze_offset_x = 0;
  int _gaze_offset_y = 0;
  int _breath_offset_y = 0;

  lv_obj_t *_container = nullptr;
  lv_obj_t *_eye = nullptr;
  lv_obj_t *_eyelid = nullptr;
};
