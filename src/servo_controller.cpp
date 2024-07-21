#include "servo_controller.h"
#include <M5Unified.h>
#include <memory>

ServoController::ServoController() {
}

ServoController::~ServoController() {
}

void ServoController::begin(int pin_x, int pin_y, int start_x_degree,
                            int start_y_degree) {
  servo_move_end_millis = millis() + 500;
}

void ServoController::moveX(int x, uint32_t millis_for_move) {
  if (millis_for_move == 0) {
  } else {
  }
}

void ServoController::moveY(int y, uint32_t millis_for_move) {
  if (millis_for_move == 0) {
  } else {
  }
}

void ServoController::moveXY(int x, int y, uint32_t millis_for_move) {
  if (millis_for_move == 0) {
  } else {
  }
}

bool ServoController::update() {
}

bool ServoController::isMoving() {
}

void ServoController::waitForAllServosToStop() {
  while (update() && (servo_move_end_millis > millis())) {
  }
}
