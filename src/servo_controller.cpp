#include "servo_controller.h"
#include <M5Unified.h>
#include <ServoEasing.hpp>
#include <memory>

ServoController::ServoController() {
  servo_x = new ServoEasing();
  servo_y = new ServoEasing();
}

ServoController::~ServoController() {
  servo_x->detach();
  servo_y->detach();
  delete servo_x;
  delete servo_y;
}

void ServoController::begin(int pin_x, int pin_y, int start_x_degree,
                            int start_y_degree) {
  if (servo_x->attach(pin_x, start_x_degree + servo_offset_x) ==
      INVALID_SERVO) {
    M5_LOGE("Error attaching servo x");
  }
  if (servo_y->attach(pin_y, start_y_degree + servo_offset_y) ==
      INVALID_SERVO) {
    M5_LOGE("Error attaching servo y");
  }
  servo_x->setEasingType(EASE_QUADRATIC_IN_OUT);
  servo_y->setEasingType(EASE_QUADRATIC_IN_OUT);
  setSpeedForAllServos(60);

  servo_move_end_millis = millis() + 500;
}

void ServoController::moveX(int x, uint32_t millis_for_move) {
  if (millis_for_move == 0) {
    servo_x->startEaseTo(x + servo_offset_x);
  } else {
    servo_x->startEaseToD(x + servo_offset_x, millis_for_move);
    servo_move_end_millis =
        max(servo_move_end_millis, millis() + millis_for_move);
  }
}

void ServoController::moveY(int y, uint32_t millis_for_move) {
  if (millis_for_move == 0) {
    servo_y->startEaseTo(y + servo_offset_y);
  } else {
    servo_y->startEaseToD(y + servo_offset_y, millis_for_move);
    servo_move_end_millis =
        max(servo_move_end_millis, millis() + millis_for_move);
  }
}

void ServoController::moveXY(int x, int y, uint32_t millis_for_move) {
  if (millis_for_move == 0) {
    servo_x->setEaseTo(x + servo_offset_x);
    servo_y->startEaseTo(y + servo_offset_y);
  } else {
    servo_x->setEaseToD(x + servo_offset_x, millis_for_move);
    servo_y->startEaseToD(y + servo_offset_y, millis_for_move);
    servo_move_end_millis =
        max(servo_move_end_millis, millis() + millis_for_move);
  }
}

bool ServoController::update() {
  volatile bool isMoving = false;
  isMoving = isMoving || !servo_x->update();
  isMoving = isMoving || !servo_y->update();
  return isMoving;
}

bool ServoController::isMoving() {
  return servo_x->areInterruptsActive() || servo_x->isMoving() ||
         servo_y->areInterruptsActive() || servo_y->isMoving();
  // Note: treat true even if the survo does not start yet.
}

void ServoController::waitForAllServosToStop() {
  while (update() && (servo_move_end_millis > millis())) {
    // M5_LOGI("%d %d", servo_x->getCurrentAngle(), servo_y->getCurrentAngle());
    delay(REFRESH_INTERVAL_MILLIS);
  }
}
