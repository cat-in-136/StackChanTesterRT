#pragma once

#include "AvatarTheme.h"
#include "Expression.h"

struct AvatarPoint {
  int x = 0;
  int y = 0;
};

class Element {
public:
  virtual ~Element() = default;

  virtual void setPosition(const AvatarPoint &position) {
    _position.x = AvatarTheme::clampInt(position.x, -100, 100);
    _position.y = AvatarTheme::clampInt(position.y, -100, 100);
  }

  AvatarPoint getPosition() const { return _position; }

  virtual void setRotation(int rotation) {
    _rotation = AvatarTheme::clampInt(rotation, 0, 3600);
  }

  int getRotation() const { return _rotation; }

  virtual void setEmotion(Expression emotion) { (void)emotion; }

  virtual void setIgnoreEmotion(bool ignore) { _ignore_emotion = ignore; }

  bool getIgnoreEmotion() const { return _ignore_emotion; }

  virtual void setVisible(bool visible) { _visible = visible; }

  bool getVisible() const { return _visible; }

  virtual void update() {}

protected:
  AvatarPoint _position;
  int _rotation = 0;
  bool _visible = true;
  bool _ignore_emotion = false;
};
