#include "DefaultEffect.h"

#include <cmath>

#include "AvatarTheme.h"

namespace {
constexpr int EFFECT_X = 240;
constexpr int EFFECT_Y = 0;
constexpr int EFFECT_W = 80;
constexpr int EFFECT_H = 132;
} // namespace

DefaultEffect::DefaultEffect(lv_obj_t *parent, lv_color_t primary_color,
                             lv_color_t secondary_color)
    : _primary_color(primary_color), _secondary_color(secondary_color) {
  _obj = AvatarTheme::createContainer(parent);
  lv_obj_set_size(_obj, EFFECT_W, EFFECT_H);
  lv_obj_align(_obj, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_opa(_obj, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(_obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(_obj, drawEventCb, LV_EVENT_DRAW_MAIN, this);
}

DefaultEffect::~DefaultEffect() {
  if (_obj != nullptr) {
    lv_obj_delete(_obj);
    _obj = nullptr;
  }
}

void DefaultEffect::setExpression(Expression expression) {
  if (expression == _expression) {
    return;
  }
  _expression = expression;
  if (_obj != nullptr) {
    lv_obj_invalidate(_obj);
  }
}

void DefaultEffect::setBreath(float breath) {
  if (breath == _breath) {
    return;
  }
  _breath = breath;
  if (_obj != nullptr && _expression != Expression::Neutral) {
    lv_obj_invalidate(_obj);
  }
}

void DefaultEffect::setColors(lv_color_t primary_color,
                              lv_color_t secondary_color) {
  _primary_color = primary_color;
  _secondary_color = secondary_color;
  if (_obj != nullptr) {
    lv_obj_invalidate(_obj);
  }
}

void DefaultEffect::drawEventCb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
    return;
  }
  DefaultEffect *self = (DefaultEffect *)lv_event_get_user_data(event);
  if (self != nullptr) {
    self->draw(lv_event_get_layer(event));
  }
}

void DefaultEffect::fillCircle(lv_layer_t *layer, int x, int y, int radius,
                               lv_color_t color) {
  if (radius <= 0) {
    return;
  }
  lv_draw_fill_dsc_t dsc;
  lv_draw_fill_dsc_init(&dsc);
  dsc.color = color;
  dsc.opa = LV_OPA_COVER;
  dsc.radius = LV_RADIUS_CIRCLE;

  lv_area_t area;
  area.x1 = x - radius;
  area.y1 = y - radius;
  area.x2 = x + radius;
  area.y2 = y + radius;
  lv_draw_fill(layer, &dsc, &area);
}

void DefaultEffect::fillRect(lv_layer_t *layer, int x, int y, int width,
                             int height, lv_color_t color) {
  if (width <= 0 || height <= 0) {
    return;
  }
  lv_draw_fill_dsc_t dsc;
  lv_draw_fill_dsc_init(&dsc);
  dsc.color = color;
  dsc.opa = LV_OPA_COVER;
  dsc.radius = 0;

  lv_area_t area;
  area.x1 = x;
  area.y1 = y;
  area.x2 = x + width - 1;
  area.y2 = y + height - 1;
  lv_draw_fill(layer, &dsc, &area);
}

void DefaultEffect::drawCircle(lv_layer_t *layer, int x, int y, int radius,
                               lv_color_t color) {
  if (radius <= 0) {
    return;
  }
  lv_draw_border_dsc_t dsc;
  lv_draw_border_dsc_init(&dsc);
  dsc.color = color;
  dsc.opa = LV_OPA_COVER;
  dsc.width = 1;
  dsc.radius = LV_RADIUS_CIRCLE;
  dsc.side = LV_BORDER_SIDE_FULL;

  lv_area_t area;
  area.x1 = x - radius;
  area.y1 = y - radius;
  area.x2 = x + radius;
  area.y2 = y + radius;
  lv_draw_border(layer, &dsc, &area);
}

void DefaultEffect::fillTriangle(lv_layer_t *layer, int x1, int y1, int x2,
                                 int y2, int x3, int y3, lv_color_t color) {
  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);
  dsc.color = color;
  dsc.opa = LV_OPA_COVER;
  dsc.p[0].x = x1;
  dsc.p[0].y = y1;
  dsc.p[1].x = x2;
  dsc.p[1].y = y2;
  dsc.p[2].x = x3;
  dsc.p[2].y = y3;
  lv_draw_triangle(layer, &dsc);
}

void DefaultEffect::drawBubbleMark(lv_layer_t *layer, int x, int y, int radius,
                                   float offset) {
  int r = radius + (int)(radius * 0.2f * offset);
  drawCircle(layer, x, y, r, _primary_color);
  drawCircle(layer, x - r / 4, y - r / 4, r / 4, _primary_color);
}

void DefaultEffect::drawSweatMark(lv_layer_t *layer, int x, int y, int radius,
                                  float offset) {
  y += (int)(5.0f * offset);
  int r = radius + (int)(radius * 0.2f * offset);
  fillCircle(layer, x, y, r, _primary_color);
  int a = (int)(sqrtf(3.0f) * r / 2.0f);
  fillTriangle(layer, x, y - r * 2, x - a, y - r / 2, x + a, y - r / 2,
               _primary_color);
}

void DefaultEffect::drawChillMark(lv_layer_t *layer, int x, int y, int radius,
                                  float offset) {
  int h = radius + (int)fabsf(radius * 0.2f * offset);
  fillRect(layer, x - radius / 2, y, 3, h / 2, _primary_color);
  fillRect(layer, x, y, 3, h * 3 / 4, _primary_color);
  fillRect(layer, x + radius / 2, y, 3, h, _primary_color);
}

void DefaultEffect::drawAngerMark(lv_layer_t *layer, int x, int y, int radius,
                                  float offset) {
  int r = radius + (int)fabsf(radius * 0.4f * offset);
  int w = (r * 2) / 3;
  int h = r * 2;
  fillRect(layer, x - r / 3, y - r, w, h, _primary_color);
  fillRect(layer, x - r, y - r / 3, r * 2, (r * 2) / 3, _primary_color);
  fillRect(layer, x - r / 3 + 2, y - r, w - 4, h, _secondary_color);
  fillRect(layer, x - r, y - r / 3 + 2, r * 2, (r * 2) / 3 - 4,
           _secondary_color);
}

void DefaultEffect::drawHeartMark(lv_layer_t *layer, int x, int y, int radius,
                                 float offset) {
  int r = radius + (int)(radius * 0.4f * offset);
  fillCircle(layer, x - r / 2, y, r / 2, _primary_color);
  fillCircle(layer, x + r / 2, y, r / 2, _primary_color);
  int a = (int)(sqrtf(2.0f) * r / 4.0f);
  fillTriangle(layer, x, y, x - r / 2 - a, y + a, x + r / 2 + a, y + a,
               _primary_color);
  fillTriangle(layer, x, y + r / 2 + 2 * a, x - r / 2 - a, y + a,
               x + r / 2 + a, y + a, _primary_color);
}

void DefaultEffect::draw(lv_layer_t *layer) {
  if (_expression == Expression::Neutral) {
    return;
  }

  lv_area_t area;
  lv_obj_get_coords(_obj, &area);
  int ox = area.x1 - EFFECT_X;
  int oy = area.y1 - EFFECT_Y;

  switch (_expression) {
  case Expression::Doubt:
    drawSweatMark(layer, ox + 290, oy + 110, 7, -_breath);
    break;
  case Expression::Angry:
    drawAngerMark(layer, ox + 280, oy + 50, 12, _breath);
    break;
  case Expression::Happy:
    drawHeartMark(layer, ox + 280, oy + 50, 12, _breath);
    break;
  case Expression::Sad:
    drawChillMark(layer, ox + 270, oy + 0, 30, _breath);
    break;
  case Expression::Sleepy:
    drawBubbleMark(layer, ox + 290, oy + 40, 10, _breath);
    drawBubbleMark(layer, ox + 270, oy + 52, 6, -_breath);
    break;
  default:
    break;
  }
}
