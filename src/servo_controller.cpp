#include "servo_controller.h"
#include <Dynamixel2Arduino.h>
#include <M5Unified.h>

ServoController::ServoController(HardwareSerial &serial) : serial(serial) {}

ServoController::~ServoController() {}

void ServoController::begin(uint8_t id_x, uint8_t id_y, int start_x_degree,
                            int start_y_degree) {
  this->id_x = id_x;
  this->id_y = id_y;

  const auto baudRate = serial.baudRate();
  dxl = Dynamixel2Arduino(serial);
  dxl.setPortProtocolVersion(2.0f);
  dxl.begin(baudRate);
  {
    static const size_t PING_INFO_COUNT = 2;
    DYNAMIXEL::InfoFromPing_t ping_info[PING_INFO_COUNT] = {0xFF};
    bool id_x_found = false;
    bool id_y_found = false;

    if (dxl.ping(DXL_BROADCAST_ID, ping_info, PING_INFO_COUNT)) {
      for (const auto info : ping_info) {
        if (info.id == id_x) {
          id_x_found = true;
          dxl.ledOn(id_x);
        }
        if (info.id == id_y) {
          id_y_found = true;
          dxl.ledOn(id_y);
        }
        M5_LOGI("ID : %d, Model Number: 0x%04u, Ver:%d\n", info.id,
                info.model_number, info.firmware_version);
      }
    }

    if (!id_x_found) {
      M5_LOGE("DYNAMIXEL for X (ID=%d) not found!", id_x);
    }
    if (!id_y_found) {
      M5_LOGE("DYNAMIXEL for Y (ID=%d) not found!", id_y);
    }
  }

  {
    dxl.torqueOff(id_x);
    dxl.torqueOff(id_y);
    M5_LOGD("operating mode");
    dxl.setOperatingMode(id_x, OP_EXTENDED_POSITION);
    dxl.setOperatingMode(id_y, OP_EXTENDED_POSITION);

    dxl.writeControlTableItem(ControlTableItem::PROFILE_ACCELERATION, id_x, 20);
    dxl.writeControlTableItem(ControlTableItem::PROFILE_ACCELERATION, id_y, 20);
    dxl.writeControlTableItem(ControlTableItem::PROFILE_VELOCITY, id_x, 100);
    dxl.writeControlTableItem(ControlTableItem::PROFILE_VELOCITY, id_y, 20);

    M5_LOGD("enable torque");
    dxl.torqueOn(id_x);
    dxl.torqueOn(id_y);

    dxl.ledOff(id_x);
    dxl.ledOff(id_y);
  }

  update();
}

void ServoController::moveX(float x) {
  dxl.setGoalPosition(id_x, x + servo_offset_x, UNIT_DEGREE);
  is_moving_x = true;
}

void ServoController::moveY(float y) {
  dxl.setGoalPosition(id_y, y + servo_offset_y, UNIT_DEGREE);
  is_moving_y = true;
}

void ServoController::moveXY(float x, float y) {
  moveX(x);
  moveY(y);
}

bool ServoController::update() {
  const auto status_x =
      dxl.readControlTableItem(ControlTableItem::MOVING_STATUS, id_x);
  const auto status_y =
      dxl.readControlTableItem(ControlTableItem::MOVING_STATUS, id_y);

  is_moving_x = (status_x & 0x02) != 0x00;
  is_moving_y = (status_y & 0x02) != 0x00;

  const auto pos_x = dxl.getPresentPosition(id_x, UNIT_DEGREE);
  const auto pos_y = dxl.getPresentPosition(id_y, UNIT_DEGREE);

  // XXX DEBUG
  M5_LOGI("pos (%f,%f), status x:%02x, y:%02x, moving_x:%d,moving_y:%d", pos_x,
          pos_y, status_x, status_y, is_moving_x ? 1 : 0, is_moving_y);

  const bool is_moving = is_moving_x || is_moving_y;
  return is_moving;
}

bool ServoController::isMoving() { return is_moving_x || is_moving_y; }

void ServoController::waitForAllServosToStop() {
  while (update()) {
    delay(10);
  }
}
