#pragma once
#include <Dynamixel2Arduino.h>
#include <stdint.h>

#define DEFAULT_START_DEGREE_VALUE_X 360
#define DEFAULT_START_DEGREE_VALUE_Y 180

class ServoController {
public:
  ServoController(HardwareSerial &serial);
  ~ServoController();

  void begin(uint8_t id_x = 1, uint8_t id_y = 2,
             int start_x_degree = DEFAULT_START_DEGREE_VALUE_X,
             int start_y_degree = DEFAULT_START_DEGREE_VALUE_Y);

  void moveX(float x);
  void moveY(float y);
  void moveXY(float x, float y);

  inline float getServoOffsetX() { return servo_offset_x; };
  inline float getServoOffsetY() { return servo_offset_y; };
  inline void setServoOffsetX(float offset) { servo_offset_x = offset; };
  inline void setServoOffsetY(float offset) { servo_offset_y = offset; };

  bool update();
  bool isMoving();
  void waitForAllServosToStop();

private:
  HardwareSerial &serial;
  Dynamixel2Arduino dxl;
  uint8_t id_x;
  uint8_t id_y;

  float servo_offset_x = DEFAULT_START_DEGREE_VALUE_X;
  float servo_offset_y = DEFAULT_START_DEGREE_VALUE_Y;

  bool is_moving_x = false;
  bool is_moving_y = false;
};
