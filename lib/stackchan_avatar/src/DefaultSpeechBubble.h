#pragma once

#include "Element.h"

class DefaultSpeechBubble : public Element {
public:
  DefaultSpeechBubble(lv_obj_t *parent, lv_color_t primary_color,
                      lv_color_t secondary_color);
  ~DefaultSpeechBubble() override;

  void setSpeech(const char *text);
  void clearSpeech();
  void setVisible(bool visible) override;
  void setTextFont(const lv_font_t *font);

  void setColors(lv_color_t primary_color, lv_color_t secondary_color);

private:
  void updateBubbleWidth(const char *text);

  lv_obj_t *_container = nullptr;
  lv_obj_t *_arrow = nullptr;
  lv_obj_t *_bubble = nullptr;
  lv_obj_t *_text = nullptr;
};
