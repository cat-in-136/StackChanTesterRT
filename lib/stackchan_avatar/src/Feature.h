#pragma once

#include "Element.h"

class Feature : public Element {
public:
  virtual void setWeight(int weight) {
    _weight = AvatarTheme::clampInt(weight, 0, 100);
  }

  int getWeight() const { return _weight; }

  virtual void setSize(int size) {
    _size = AvatarTheme::clampInt(size, -100, 100);
  }

  int getSize() const { return _size; }

  virtual void setGaze(float vertical, float horizontal) {
    (void)vertical;
    (void)horizontal;
  }

  virtual void setBreathOffset(int offset_y) { (void)offset_y; }

protected:
  int _weight = 0;
  int _size = 0;
};
