#pragma once

#include <lvgl.h>

#include "Expression.h"

class DefaultEffect {
public:
  DefaultEffect(lv_obj_t *parent, lv_color_t primary_color,
                lv_color_t secondary_color);
  ~DefaultEffect();

  void setExpression(Expression expression);
  void setBreath(float breath);
  void setColors(lv_color_t primary_color, lv_color_t secondary_color);

private:
  static void drawEventCb(lv_event_t *event);
  void draw(lv_layer_t *layer);

  void fillCircle(lv_layer_t *layer, int x, int y, int radius,
                  lv_color_t color);
  void fillRect(lv_layer_t *layer, int x, int y, int width, int height,
                lv_color_t color);
  void drawCircle(lv_layer_t *layer, int x, int y, int radius,
                  lv_color_t color);
  void fillTriangle(lv_layer_t *layer, int x1, int y1, int x2, int y2, int x3,
                    int y3, lv_color_t color);

  void drawBubbleMark(lv_layer_t *layer, int x, int y, int radius,
                      float offset);
  void drawSweatMark(lv_layer_t *layer, int x, int y, int radius,
                     float offset);
  void drawChillMark(lv_layer_t *layer, int x, int y, int radius,
                     float offset);
  void drawAngerMark(lv_layer_t *layer, int x, int y, int radius,
                     float offset);
  void drawHeartMark(lv_layer_t *layer, int x, int y, int radius,
                     float offset);

  lv_obj_t *_obj = nullptr;
  lv_color_t _primary_color;
  lv_color_t _secondary_color;
  Expression _expression = Expression::Neutral;
  float _breath = 0.0f;
};
