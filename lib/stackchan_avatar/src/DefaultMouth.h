#pragma once

#include "Feature.h"

class DefaultMouth : public Feature {
public:
  DefaultMouth(lv_obj_t *parent, lv_color_t primary_color,
               lv_color_t secondary_color);
  ~DefaultMouth() override;

  void setPosition(const AvatarPoint &position) override;
  void setWeight(int weight) override;
  void setRotation(int rotation) override;
  void setVisible(bool visible) override;
  void setBreathOffset(int offset_y) override;

  void setColors(lv_color_t primary_color);

  lv_obj_t *getContainer() const { return _mouth; }

private:
  void applyPosition();
  void applyWeight();

  lv_obj_t *_mouth = nullptr;
  int _breath_offset_y = 0;
};
