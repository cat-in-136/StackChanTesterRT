#pragma once
#include <stdint.h>

#define DEFAULT_START_DEGREE_VALUE_X 90
#define DEFAULT_START_DEGREE_VALUE_Y 90

class ServoController {
public:
  ServoController();
  ~ServoController();

  void begin(int pin_x, int pin_y,
             int start_x_degree = DEFAULT_START_DEGREE_VALUE_X,
             int start_y_degree = DEFAULT_START_DEGREE_VALUE_Y);

  void moveX(int x, uint32_t millis_for_move = 0);
  void moveY(int y, uint32_t millis_for_move = 0);
  void moveXY(int x, int y, uint32_t millis_for_move = 0);

  inline int getServoOffsetX() { return servo_offset_x; };
  inline int getServoOffsetY() { return servo_offset_y; };
  inline void setServoOffsetX(int offset) { servo_offset_x = offset; };
  inline void setServoOffsetY(int offset) { servo_offset_y = offset; };

  bool update();
  bool isMoving();
  void waitForAllServosToStop();

private:
  unsigned long servo_move_end_millis = 0;

  int servo_offset_x = 0;
  int servo_offset_y = 0;
};
