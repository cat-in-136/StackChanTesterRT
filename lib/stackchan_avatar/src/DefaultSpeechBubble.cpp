#include "DefaultSpeechBubble.h"

#include <cstring>

LV_IMAGE_DECLARE(default_bubble_arrow);

namespace {
constexpr int CONTAINER_POS_X = 0;
constexpr int CONTAINER_POS_Y = 89;
constexpr int CONTAINER_W = 320;
constexpr int CONTAINER_H = 74;
constexpr int ARROW_OFFSET_X = 40;
constexpr int ARROW_OFFSET_Y = -15;
constexpr int TEXT_MARGIN_X = 20;
constexpr int BUBBLE_MIN_W = 90;
constexpr int BUBBLE_MAX_W = 340;
constexpr int BUBBLE_H = 52;
constexpr int BUBBLE_MIN_OFFSET_X = 66;
constexpr int BUBBLE_MAX_OFFSET_X = 0;
} // namespace

DefaultSpeechBubble::DefaultSpeechBubble(lv_obj_t *parent,
                                         lv_color_t primary_color,
                                         lv_color_t secondary_color) {
  _container = AvatarTheme::createContainer(parent);
  lv_obj_set_size(_container, CONTAINER_W, CONTAINER_H);
  lv_obj_set_style_bg_opa(_container, LV_OPA_TRANSP, 0);
  lv_obj_align(_container, LV_ALIGN_CENTER, CONTAINER_POS_X, CONTAINER_POS_Y);

  _arrow = lv_image_create(_container);
  lv_image_set_src(_arrow, &default_bubble_arrow);
  lv_obj_set_style_image_recolor(_arrow, primary_color, 0);
  lv_obj_set_style_image_recolor_opa(_arrow, LV_OPA_COVER, 0);
  lv_obj_align(_arrow, LV_ALIGN_CENTER, ARROW_OFFSET_X, ARROW_OFFSET_Y);

  _bubble = AvatarTheme::createContainer(_container);
  lv_obj_set_style_radius(_bubble, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_bubble, primary_color, 0);
  lv_obj_set_size(_bubble, BUBBLE_MAX_W, BUBBLE_H);
  lv_obj_align(_bubble, LV_ALIGN_CENTER, 0, 11);

  _text = lv_label_create(_bubble);
  lv_obj_set_style_text_color(_text, secondary_color, 0);
  lv_obj_set_style_text_align(_text, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(_text, CONTAINER_W - TEXT_MARGIN_X * 2);
  lv_label_set_long_mode(_text, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
  lv_obj_align(_text, LV_ALIGN_CENTER, 0, 0);

  clearSpeech();
}

DefaultSpeechBubble::~DefaultSpeechBubble() {
  if (_container != nullptr) {
    lv_obj_delete(_container);
    _container = nullptr;
    _arrow = nullptr;
    _bubble = nullptr;
    _text = nullptr;
  }
}

void DefaultSpeechBubble::updateBubbleWidth(const char *text) {
  lv_point_t text_size;
  const lv_font_t *font = lv_obj_get_style_text_font(_text, LV_PART_MAIN);
  lv_text_get_size(&text_size, text, font, 0, 0, LV_COORD_MAX,
                   LV_TEXT_FLAG_NONE);

  int bubble_width = text_size.x + TEXT_MARGIN_X * 2;
  bubble_width =
      AvatarTheme::clampInt(bubble_width, BUBBLE_MIN_W, BUBBLE_MAX_W);

  int bubble_offset_x = AvatarTheme::mapRange(
      bubble_width, BUBBLE_MIN_W, BUBBLE_MAX_W, BUBBLE_MIN_OFFSET_X,
      BUBBLE_MAX_OFFSET_X);

  lv_obj_set_width(_bubble, bubble_width);
  lv_obj_set_x(_bubble, bubble_offset_x);
}

void DefaultSpeechBubble::setSpeech(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    clearSpeech();
    return;
  }

  lv_label_set_text(_text, text);
  updateBubbleWidth(text);
  setVisible(true);
}

void DefaultSpeechBubble::clearSpeech() {
  lv_label_set_text(_text, "");
  setVisible(false);
}

void DefaultSpeechBubble::setVisible(bool visible) {
  Element::setVisible(visible);
  if (visible) {
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_container);
  } else {
    lv_obj_add_flag(_container, LV_OBJ_FLAG_HIDDEN);
  }
}

void DefaultSpeechBubble::setTextFont(const lv_font_t *font) {
  if (font != nullptr && _text != nullptr) {
    lv_obj_set_style_text_font(_text, font, 0);
  }
}

void DefaultSpeechBubble::setColors(lv_color_t primary_color,
                                    lv_color_t secondary_color) {
  lv_obj_set_style_image_recolor(_arrow, primary_color, 0);
  lv_obj_set_style_bg_color(_bubble, primary_color, 0);
  lv_obj_set_style_text_color(_text, secondary_color, 0);
}
