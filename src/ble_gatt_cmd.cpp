#include "ble_gatt_cmd.h"

#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <BLEUtils.h>
#include <M5Unified.h>
#include <cstring>

#include "servo_controller.h"

extern ServoController servo_controller;

/* ---- BLE UUIDs (StackChan spec) ---- */
#define BLE_SERVICE_UUID "e2e5e5e0-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_MOTION_UUID "e2e5e5e1-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_AVATAR_UUID "e2e5e5e2-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_CONFIG_UUID "e2e5e5e3-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_RGB_UUID "e2e5e5e4-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_BATTERY_UUID 0x2A19

#define BLE_DEVICE_NAME "StackChanRT"
#define BLE_MAX_JSON_LEN 2048
#define BLE_FRAG_HEADER_LEN 10

/* ---- Fragment header magic ---- */
/* M5 StackChan panel compatibility mapping (tenths of a degree):
 *   M5 yaw   : -1280..1280, 0 = front, + = left.
 *   M5 pitch : 0..900, 0 = front (down-most), + = up.
 * StackChan RT canonical (see servo_controller.h):
 *   RT X (yaw, ID=1)   : degrees, 0 = front, + = left.
 *   RT Y (pitch, ID=2) : degrees, 0 = front, + = down (inverted vs M5).
 * Hence: rt_x = m5_yaw / 10, rt_y = -m5_pitch / 10.
 * M5 home (0,0) maps to RT (0,0) = face-forward. */
#define BLE_M5_YAW_MIN -1280
#define BLE_M5_YAW_MAX 1280
#define BLE_M5_PITCH_MIN 0
#define BLE_M5_PITCH_MAX 900
#define FRAG_MAGIC0 0xAA
#define FRAG_MAGIC1 0x55
#define FRAG_MAGIC2 0xC3
#define FRAG_VERSION 0x01

/* ---- GATT Characteristic instances ---- */
static BLECharacteristic *pMotionChar = nullptr;
static BLECharacteristic *pAvatarChar = nullptr;
static BLECharacteristic *pConfigChar = nullptr;
static BLECharacteristic *pRgbChar = nullptr;
static BLECharacteristic *pBatteryChar = nullptr;
static BLEServer *pServer = nullptr;

/* ---- Forward declarations ---- */
class MotionCharacteristicCallbacks;
class ConfigCharacteristicCallbacks;
static void bleSendNotify(BLECharacteristic *pChar, uint8_t *data, size_t len);

/* ---- Fragment reassembly ---- */
struct FragAssembler {
  uint8_t buf[BLE_MAX_JSON_LEN];
  size_t offset = 0;
  uint16_t total_len = 0;
  uint16_t total_packets = 0;
  uint16_t expected_packet_index = 0;
  bool active = false;

  void reset() {
    offset = 0;
    total_len = 0;
    total_packets = 0;
    expected_packet_index = 0;
    active = false;
  }

  void start(uint16_t tot_len, uint16_t tot_pkts) {
    active = true;
    total_len = tot_len;
    total_packets = tot_pkts;
    expected_packet_index = 0;
    offset = 0;
  }

  bool addFragment(const uint8_t *data, size_t len) {
    if (len < BLE_FRAG_HEADER_LEN)
      return false;
    if (data[0] != FRAG_MAGIC0 || data[1] != FRAG_MAGIC1 ||
        data[2] != FRAG_MAGIC2)
      return false;
    if (data[3] != FRAG_VERSION)
      return false;

    uint16_t pkt_idx = (data[4] << 8) | data[5];
    uint16_t tot_pkts = (data[6] << 8) | data[7];
    uint16_t tot_len = (data[8] << 8) | data[9];

    if (tot_pkts == 0 || tot_len == 0 || tot_len > BLE_MAX_JSON_LEN) {
      reset();
      return false;
    }

    if (!active) {
      if (pkt_idx != 0)
        return false;
      start(tot_len, tot_pkts);
    } else if (pkt_idx == 0) {
      // Sender resends first packet: restart assembly.
      start(tot_len, tot_pkts);
    } else if (pkt_idx != expected_packet_index || tot_pkts != total_packets ||
               tot_len != total_len) {
      reset();
      return false;
    }

    size_t payload_len = len - BLE_FRAG_HEADER_LEN;
    if (payload_len == 0) {
      reset();
      return false;
    }
    if (offset + payload_len > total_len ||
        offset + payload_len > BLE_MAX_JSON_LEN) {
      reset();
      return false;
    }

    memcpy(buf + offset, data + BLE_FRAG_HEADER_LEN, payload_len);
    offset += payload_len;
    expected_packet_index = pkt_idx + 1;

    if (expected_packet_index < total_packets) {
      if (offset >= total_len) {
        // Got all bytes before all packets arrived: malformed.
        reset();
        return false;
      }
      return false; // need more packets
    }

    if (offset != total_len) {
      reset();
      return false;
    }

    return true;
  }

  bool isComplete() const { return active && offset >= total_len; }
  size_t getLength() const { return total_len; }
  const uint8_t *getData() const { return buf; }
};

static FragAssembler motion_assembler;
static FragAssembler config_assembler;

/* ---- Forward declarations for callbacks ---- */
static void handleBleMotionData(const char *json, size_t len);
static void handleBleConfigData(const char *json, size_t len);

/* ---- Characteristic callbacks ---- */
class MotionCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0 || data == nullptr)
      return;

    if (len >= BLE_FRAG_HEADER_LEN && data[0] == FRAG_MAGIC0 &&
        data[1] == FRAG_MAGIC1 && data[2] == FRAG_MAGIC2) {
      if (motion_assembler.addFragment(data, len)) {
        size_t tot = motion_assembler.getLength();
        char *json_copy = new char[tot + 1];
        memcpy(json_copy, motion_assembler.getData(), tot);
        json_copy[tot] = 0;
        motion_assembler.reset();
        handleBleMotionData(json_copy, tot);
        delete[] json_copy;
      }
    } else {
      char *json_copy = new char[len + 1];
      memcpy(json_copy, data, len);
      json_copy[len] = 0;
      handleBleMotionData(json_copy, len);
      delete[] json_copy;
    }
  }
};

class ConfigCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0 || data == nullptr)
      return;

    if (len >= BLE_FRAG_HEADER_LEN && data[0] == FRAG_MAGIC0 &&
        data[1] == FRAG_MAGIC1 && data[2] == FRAG_MAGIC2) {
      if (config_assembler.addFragment(data, len)) {
        size_t tot = config_assembler.getLength();
        char *json_copy = new char[tot + 1];
        memcpy(json_copy, config_assembler.getData(), tot);
        json_copy[tot] = 0;
        config_assembler.reset();
        handleBleConfigData(json_copy, tot);
        delete[] json_copy;
      }
    } else {
      char *json_copy = new char[len + 1];
      memcpy(json_copy, data, len);
      json_copy[len] = 0;
      handleBleConfigData(json_copy, len);
      delete[] json_copy;
    }
  }
};

class RgbCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    (void)pCharacteristic;
  }
};

class AvatarCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    (void)pCharacteristic;
  }
};

/* ---- Notify helper ---- */
static void bleSendNotify(BLECharacteristic *pChar, uint8_t *data, size_t len) {
  if (pChar == nullptr)
    return;
  pChar->setValue(data, len);
  pChar->notify();
}

static void bleSendNotifyStr(BLECharacteristic *pChar, const char *str) {
  size_t slen = strlen(str);
  uint8_t *buf = new uint8_t[slen];
  memcpy(buf, str, slen);
  bleSendNotify(pChar, buf, slen);
  delete[] buf;
}

/* ---- Handle BLE Motion Data ---- */
static void handleBleMotionData(const char *json, size_t len) {
  (void)len;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error)
    return;

  const char *type = doc["type"];
  if (type == nullptr || strcmp(type, "bleMotion") != 0)
    return;

  JsonObject pitchServo = doc["pitchServo"];
  JsonObject yawServo = doc["yawServo"];

  if (pitchServo.isNull() && yawServo.isNull())
    return;

  // M5 panel sends at most one of angle/rotate per servo (see
  // MotionDataItem.toJson). RT has no velocity-rotate equivalent here, so a
  // rotate request moves nothing on that axis instead of being mistaken for
  // an angle (e.g. rotate=800 must not become 80 deg).
  bool has_yaw_angle = false, has_pitch_angle = false;
  int m5_yaw = 0, m5_pitch = 0;
  int speed = 500;

  if (!yawServo.isNull()) {
    if (!yawServo["rotate"].isNull() && yawServo["rotate"].as<int>() != 0) {
      // continuous-rotation request: hold yaw (no-op on RT).
    } else if (!yawServo["angle"].isNull()) {
      m5_yaw = yawServo["angle"].as<int>();
      has_yaw_angle = true;
    }
    if (!yawServo["speed"].isNull())
      speed = yawServo["speed"].as<int>();
  }
  if (!pitchServo.isNull()) {
    if (!pitchServo["rotate"].isNull() && pitchServo["rotate"].as<int>() != 0) {
      // pitch has no rotate mode on M5 either: hold pitch.
    } else if (!pitchServo["angle"].isNull()) {
      m5_pitch = pitchServo["angle"].as<int>();
      has_pitch_angle = true;
    }
    if (!pitchServo["speed"].isNull())
      speed = pitchServo["speed"].as<int>();
  }
  (void)speed; // RT uses fixed PROFILE_VELOCITY (see ServoController::begin)

  if (!has_yaw_angle && !has_pitch_angle)
    return;

  // Clamp in M5 panel units before converting (cf. motion.dart sliders:
  // yaw -1280..1280, pitch 0..900).
  if (m5_yaw < BLE_M5_YAW_MIN)
    m5_yaw = BLE_M5_YAW_MIN;
  if (m5_yaw > BLE_M5_YAW_MAX)
    m5_yaw = BLE_M5_YAW_MAX;
  if (m5_pitch < BLE_M5_PITCH_MIN)
    m5_pitch = BLE_M5_PITCH_MIN;
  if (m5_pitch > BLE_M5_PITCH_MAX)
    m5_pitch = BLE_M5_PITCH_MAX;

  // Only drive the axes present in the JSON (M5 update_from_json behaves the
  // same); a missing axis must hold instead of snapping to 0.
  if (has_yaw_angle)
    servo_controller.moveX(m5_yaw / 10.0f);
  if (has_pitch_angle)
    servo_controller.moveY(-m5_pitch / 10.0f); // M5 +=up, RT +=down
}

/* ---- Handle BLE Config Data ---- */
static void handleBleConfigData(const char *json, size_t len) {
  (void)len;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error)
    return;

  const char *cmd = doc["cmd"];
  if (cmd == nullptr)
    return;

  if (strcmp(cmd, "handshake") == 0) {
    const char *data = doc["data"];
    if (data != nullptr) {
      JsonDocument resp;
      resp["cmd"] = "notifyState";
      resp["data"]["type"] = 4;
      resp["data"]["state"] = data;
      char buf[256];
      serializeJson(resp, buf, sizeof(buf));
      bleSendNotifyStr(pConfigChar, buf);
    }
  } else if (strcmp(cmd, "getWifiStatus") == 0) {
    JsonDocument resp;
    resp["cmd"] = "notifyState";
    resp["data"]["type"] = 1;
    resp["data"]["state"] = "wifiConnected";
    char buf[256];
    serializeJson(resp, buf, sizeof(buf));
    bleSendNotifyStr(pConfigChar, buf);
  } else if (strcmp(cmd, "setWifi") == 0) {
    JsonDocument resp;
    resp["cmd"] = "notifyState";
    resp["data"]["type"] = 1;
    resp["data"]["state"] = "wifiConnected";
    char buf[256];
    serializeJson(resp, buf, sizeof(buf));
    bleSendNotifyStr(pConfigChar, buf);
  }
}

/* ---- BLE Setup ---- */
class MyServerCallbacks : public BLEServerCallbacks {
  void onDisconnect(BLEServer *pServer) override {
    pServer->startAdvertising();
  }
};

void setupBLE(void) {
  M5_LOGI("Starting BLE server...");

  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(247);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(BLEUUID(BLE_SERVICE_UUID));

  pMotionChar = pService->createCharacteristic(
      BLEUUID(BLE_CHAR_MOTION_UUID), BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE |
                                         BLECharacteristic::PROPERTY_NOTIFY);
  pMotionChar->addDescriptor(new BLE2902());
  pMotionChar->setCallbacks(new MotionCharacteristicCallbacks());

  pAvatarChar = pService->createCharacteristic(
      BLEUUID(BLE_CHAR_AVATAR_UUID), BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE |
                                         BLECharacteristic::PROPERTY_NOTIFY);
  pAvatarChar->addDescriptor(new BLE2902());
  pAvatarChar->setCallbacks(new AvatarCharacteristicCallbacks());

  pConfigChar = pService->createCharacteristic(
      BLEUUID(BLE_CHAR_CONFIG_UUID), BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE |
                                         BLECharacteristic::PROPERTY_NOTIFY);
  pConfigChar->addDescriptor(new BLE2902());
  pConfigChar->setCallbacks(new ConfigCharacteristicCallbacks());

  pRgbChar = pService->createCharacteristic(
      BLEUUID(BLE_CHAR_RGB_UUID), BLECharacteristic::PROPERTY_READ |
                                      BLECharacteristic::PROPERTY_WRITE |
                                      BLECharacteristic::PROPERTY_NOTIFY);
  pRgbChar->addDescriptor(new BLE2902());
  pRgbChar->setCallbacks(new RgbCharacteristicCallbacks());

  pBatteryChar = pService->createCharacteristic(
      BLEUUID((uint16_t)BLE_CHAR_BATTERY_UUID),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pBatteryChar->addDescriptor(new BLE2902());
  uint8_t battery_level = 96;
  pBatteryChar->setValue(&battery_level, 1);

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLEUUID(BLE_SERVICE_UUID));
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  M5_LOGI("BLE server started. Waiting for connections...");
}

void teardownBLE(void) {
  if (pServer != nullptr) {
    BLEServer *srv = pServer;
    pServer = nullptr;
    delete srv;
  }
  pMotionChar = nullptr;
  pAvatarChar = nullptr;
  pConfigChar = nullptr;
  pRgbChar = nullptr;
  pBatteryChar = nullptr;
  BLEDevice::deinit();
  M5_LOGI("BLE server stopped.");
}

void processBLE(void) {
  /* Battery is a static value readable via the READ property; no periodic
   * notifications are needed. notify() with no connected client can stall the
   * loop, so never push from here. */
}
