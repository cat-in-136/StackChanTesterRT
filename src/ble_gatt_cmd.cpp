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
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "servo_controller.h"

extern ServoController servo_controller;

/* ---- BLE UUIDs (StackChan spec) ---- */
#define BLE_SERVICE_UUID "e2e5e5e0-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_MOTION_UUID "e2e5e5e1-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_AVATAR_UUID "e2e5e5e2-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_CONFIG_UUID "e2e5e5e3-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_RGB_UUID "e2e5e5e4-1234-5678-1234-56789abcdef0"
/* StackChanRT dedicated channel (frozen e1/e2 compat; all RT commands live
 * here) */
#define BLE_CHAR_RT_UUID "e2e5e5ef-1234-5678-1234-56789abcdef0"
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
static BLECharacteristic *pRtChar = nullptr;
static BLECharacteristic *pBatteryChar = nullptr;
static BLEServer *pServer = nullptr;

/* ---- Forward declarations ---- */
class MotionCharacteristicCallbacks;
class ConfigCharacteristicCallbacks;
class RtCharacteristicCallbacks;
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
static FragAssembler avatar_assembler;
static FragAssembler rt_assembler;

/* ---- Forward declarations for callbacks ---- */
static void handleBleMotionData(const char *json, size_t len);
static void handleBleConfigData(const char *json, size_t len);
static void handleBleAvatarData(const char *json, size_t len);
static void handleBleRtData(const char *json, size_t len);

/* ---- Deferred execution queue ----
 * GATT onWrite runs in the BLE host task, NOT in loop(). Executing Dynamixel
 * calls there (shared HardwareSerial with loop()'s servo_controller.update())
 * or Avatar calls (setExpression suspends/resumes the draw task) races with
 * the loop task and the Avatar draw task, corrupting state and panicking
 * (Double exception). So onWrite only enqueues a copy; processBLE() (called
 * from loop()) does the real work in the loop task. */
enum BleCmdTag : uint8_t {
  BLE_CMD_MOTION = 0,
  BLE_CMD_AVATAR,
  BLE_CMD_CONFIG,
  BLE_CMD_RT,
};
struct BleCmdItem {
  uint8_t tag;
  char *json; // malloc'ed NUL-terminated copy, freed by processBLE()
};
#define BLE_CMD_QUEUE_DEPTH 8
static QueueHandle_t bleCmdQueue = nullptr;

static void bleEnqueue(uint8_t tag, const uint8_t *data, size_t len) {
  if (bleCmdQueue == nullptr || data == nullptr || len == 0)
    return;
  if (len > BLE_MAX_JSON_LEN) {
    M5_LOGD("ble cmd too long (%u), dropped", (unsigned)len);
    return;
  }
  char *copy = (char *)malloc(len + 1);
  if (copy == nullptr)
    return;
  memcpy(copy, data, len);
  copy[len] = 0;
  BleCmdItem item;
  item.tag = tag;
  item.json = copy;
  if (xQueueSend(bleCmdQueue, &item, 0) != pdTRUE) {
    free(copy);
    M5_LOGD("ble queue full, dropped tag=%d", tag);
  }
}

/* ---- Characteristic callbacks (enqueue only; see queue note above) ---- */
class MotionCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0 || data == nullptr)
      return;

    if (len >= BLE_FRAG_HEADER_LEN && data[0] == FRAG_MAGIC0 &&
        data[1] == FRAG_MAGIC1 && data[2] == FRAG_MAGIC2) {
      if (motion_assembler.addFragment(data, len)) {
        bleEnqueue(BLE_CMD_MOTION, motion_assembler.getData(),
                   motion_assembler.getLength());
        motion_assembler.reset();
      }
    } else {
      bleEnqueue(BLE_CMD_MOTION, data, len);
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
        bleEnqueue(BLE_CMD_CONFIG, config_assembler.getData(),
                   config_assembler.getLength());
        config_assembler.reset();
      }
    } else {
      bleEnqueue(BLE_CMD_CONFIG, data, len);
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
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0 || data == nullptr)
      return;

    if (len >= BLE_FRAG_HEADER_LEN && data[0] == FRAG_MAGIC0 &&
        data[1] == FRAG_MAGIC1 && data[2] == FRAG_MAGIC2) {
      if (avatar_assembler.addFragment(data, len)) {
        bleEnqueue(BLE_CMD_AVATAR, avatar_assembler.getData(),
                   avatar_assembler.getLength());
        avatar_assembler.reset();
      }
    } else {
      bleEnqueue(BLE_CMD_AVATAR, data, len);
    }
  }
};

/* StackChanRT dedicated characteristic (...ef);
 * all StackChanRT commands live here with {"cmd": ...} dispatch. */
class RtCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0 || data == nullptr)
      return;

    if (len >= BLE_FRAG_HEADER_LEN && data[0] == FRAG_MAGIC0 &&
        data[1] == FRAG_MAGIC1 && data[2] == FRAG_MAGIC2) {
      if (rt_assembler.addFragment(data, len)) {
        bleEnqueue(BLE_CMD_RT, rt_assembler.getData(),
                   rt_assembler.getLength());
        rt_assembler.reset();
      }
    } else {
      bleEnqueue(BLE_CMD_RT, data, len);
    }
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

/* ---- Handle BLE Avatar Data ----
 * StackChan spec (cf. references/m5stack/StackChan/firmware
 * main/stackchan/json/json_helper.cpp avatar::update_from_json):
 *   { "leftEye": { "weight": 0..100 },
 *     "rightEye": { "weight": 0..100 },
 *     "mouth": { "weight": 0..100 } }
 * API normalization: stackchan::avatar::Feature::setWeight(int 0..100) maps
 * onto m5avatar::Avatar open ratios (float 0.0..1.0):
 *   leftEye.weight  -> setLeftEyeOpenRatio(weight / 100)
 *   rightEye.weight -> setRightEyeOpenRatio(weight / 100)
 *   mouth.weight    -> setMouthOpenRatio(weight / 100)
 * Other feature fields (x/y/rotation/size) have no per-feature equivalent in
 * the m5avatar API and are ignored. Expression is intentionally untouched.
 *
 * NOTE: an explicit eye weight also disables auto-blink
 * (m5avatar::Avatar::facialLoop would otherwise overwrite the eye open ratio
 * every few seconds, so the BLE setting would never stick). */
static bool bleAvatarWeight(JsonObject obj, float *out_ratio) {
  if (obj.isNull() || out_ratio == nullptr)
    return false;
  JsonVariant w = obj["weight"];
  if (!w.is<int>() && !w.is<float>())
    return false;
  float weight = w.as<float>();
  if (weight < 0.0f)
    weight = 0.0f;
  if (weight > 100.0f)
    weight = 100.0f;
  *out_ratio = weight / 100.0f;
  return true;
}

static void handleBleAvatarData(const char *json, size_t len) {
  (void)len;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error)
    return;

  float ratio = 0.0f;
  bool has_eye = false;

  if (!doc["leftEye"].isNull()) {
    JsonObject leftEye = doc["leftEye"];
    if (bleAvatarWeight(leftEye, &ratio)) {
      avatar.setLeftEyeOpenRatio(ratio);
      has_eye = true;
    }
  }
  if (!doc["rightEye"].isNull()) {
    JsonObject rightEye = doc["rightEye"];
    if (bleAvatarWeight(rightEye, &ratio)) {
      avatar.setRightEyeOpenRatio(ratio);
      has_eye = true;
    }
  }
  if (has_eye) {
    avatar.setIsAutoBlink(false);
  }

  if (!doc["mouth"].isNull()) {
    JsonObject mouth = doc["mouth"];
    if (bleAvatarWeight(mouth, &ratio)) {
      avatar.setMouthOpenRatio(ratio);
    }
  }
}

/* ---- Handle StackChanRT Data (...ef) ----
 * All StackChanRT commands live here.
 *   {"cmd":"rtMotion","yaw":{angle,speed},"pitch":{...}} (M5 units)
 *   {"cmd":"rtAvatar","leftEye":{x,y,rotation,weight,size},...}
 *   {"cmd":"rtExpression","expression":"Happy"|0..5}
 * Unknown cmd -> ignore + log (forward-compatible for future RT cmds). */
static int bleRtClampInt(JsonObject obj, const char *key, int lo, int hi,
                         int fallback, bool *present) {
  JsonVariant v = obj[key];
  if (v.isNull() || (!v.is<int>() && !v.is<float>())) {
    if (present)
      *present = false;
    return fallback;
  }
  if (present)
    *present = true;
  int iv = v.as<int>();
  if (iv < lo)
    iv = lo;
  if (iv > hi)
    iv = hi;
  return iv;
}

static void bleRtDriveServo(bool is_yaw, JsonObject servo) {
  if (servo.isNull())
    return;
  bool has_angle = false;
  const int lo = is_yaw ? BLE_M5_YAW_MIN : BLE_M5_PITCH_MIN;
  const int hi = is_yaw ? BLE_M5_YAW_MAX : BLE_M5_PITCH_MAX;
  int angle = bleRtClampInt(servo, "angle", lo, hi, 0, &has_angle);
  if (!has_angle)
    return;

  bool has_speed = false;
  int speed = bleRtClampInt(servo, "speed", 1, 1000, 500, &has_speed);

  float rt_deg =
      is_yaw ? (angle / 10.0f) : (-angle / 10.0f); // M5 +=up, RT +=down
  if (has_speed) {
    if (is_yaw)
      servo_controller.setSpeedX(speed);
    else
      servo_controller.setSpeedY(speed);
  }
  if (is_yaw)
    servo_controller.moveX(rt_deg);
  else
    servo_controller.moveY(rt_deg);
}

/* rtExpression value: case-insensitive name or 0..5
 * (Happy,Angry,Sad,Doubt,Sleepy,Neutral).
 * NOTE: BLE is only active in BleRemote mode where the Avatar task runs, so
 * setExpression()->suspend() is safe here (never call from LVGL modes). */
static bool bleRtApplyExpression(JsonVariant v) {
  if (v.isNull())
    return false;
  m5avatar::Expression exp = m5avatar::Expression::Neutral;
  if (v.is<int>() || v.is<float>()) {
    switch (v.as<int>()) {
    case 0:
      exp = m5avatar::Expression::Happy;
      break;
    case 1:
      exp = m5avatar::Expression::Angry;
      break;
    case 2:
      exp = m5avatar::Expression::Sad;
      break;
    case 3:
      exp = m5avatar::Expression::Doubt;
      break;
    case 4:
      exp = m5avatar::Expression::Sleepy;
      break;
    case 5:
      exp = m5avatar::Expression::Neutral;
      break;
    default:
      M5_LOGD("expression index out of range");
      return false;
    }
  } else if (v.is<const char *>() || v.is<String>()) {
    char buf[16];
    strncpy(buf, v.as<const char *>(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char *p = buf; *p; p++)
      *p = tolower((unsigned char)*p);
    if (strcmp(buf, "happy") == 0)
      exp = m5avatar::Expression::Happy;
    else if (strcmp(buf, "angry") == 0)
      exp = m5avatar::Expression::Angry;
    else if (strcmp(buf, "sad") == 0)
      exp = m5avatar::Expression::Sad;
    else if (strcmp(buf, "doubt") == 0)
      exp = m5avatar::Expression::Doubt;
    else if (strcmp(buf, "sleepy") == 0)
      exp = m5avatar::Expression::Sleepy;
    else if (strcmp(buf, "neutral") == 0)
      exp = m5avatar::Expression::Neutral;
    else {
      M5_LOGD("expression unknown name");
      return false;
    }
  } else {
    return false;
  }
  avatar.setExpression(exp);
  return true;
}

static void handleBleRtAvatar(JsonDocument &doc) {
  float ratio = 0.0f;
  bool has_eye = false;

  const char *keys[2] = {"leftEye", "rightEye"};
  for (int i = 0; i < 2; i++) {
    if (doc[keys[i]].isNull())
      continue;
    JsonObject eye = doc[keys[i]];
    if (bleAvatarWeight(eye, &ratio)) {
      if (i == 0)
        avatar.setLeftEyeOpenRatio(ratio);
      else
        avatar.setRightEyeOpenRatio(ratio);
      has_eye = true;
    }
    // x/y -> gaze.
    bool has_x = false, has_y = false;
    int x = bleRtClampInt(eye, "x", -100, 100, 0, &has_x);
    int y = bleRtClampInt(eye, "y", -100, 100, 0, &has_y);
    if (has_x || has_y) {
      float h = x / 100.0f;
      float v = y / 100.0f;
      if (i == 0)
        avatar.setLeftGaze(v, h);
      else
        avatar.setRightGaze(v, h);
    }
    bool has_rot = false;
    int rot = bleRtClampInt(eye, "rotation", -180, 180, 0, &has_rot);
    if (has_rot)
      avatar.setRotation(rot * 3.14159265f / 180.0f); // global, last wins
    bool has_size = false;
    int sz = bleRtClampInt(eye, "size", -100, 100, 0, &has_size);
    if (has_size) {
      float s = 1.0f + sz / 100.0f;
      if (s < 0.1f)
        s = 0.1f;
      if (s > 2.0f)
        s = 2.0f;
      avatar.setScale(s); // global, last wins
    }
  }
  if (has_eye)
    avatar.setIsAutoBlink(false);

  if (!doc["mouth"].isNull()) {
    JsonObject mouth = doc["mouth"];
    if (bleAvatarWeight(mouth, &ratio))
      avatar.setMouthOpenRatio(ratio);
    bool has_rot = false;
    int rot = bleRtClampInt(mouth, "rotation", -180, 180, 0, &has_rot);
    if (has_rot)
      avatar.setRotation(rot * 3.14159265f / 180.0f);
    bool has_size = false;
    int sz = bleRtClampInt(mouth, "size", -100, 100, 0, &has_size);
    if (has_size) {
      float s = 1.0f + sz / 100.0f;
      if (s < 0.1f)
        s = 0.1f;
      if (s > 2.0f)
        s = 2.0f;
      avatar.setScale(s);
    }
  }
  if (!doc["expression"].isNull())
    bleRtApplyExpression(doc["expression"]);
}

static void handleBleRtData(const char *json, size_t len) {
  (void)len;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    M5_LOGD("rt JSON parse failed");
    return;
  }
  const char *cmd = doc["cmd"];
  if (cmd == nullptr) {
    M5_LOGD("rt missing cmd");
    return;
  }
  if (strcmp(cmd, "rtMotion") == 0) {
    if (!doc["yaw"].isNull()) {
      JsonObject yaw = doc["yaw"];
      bleRtDriveServo(true, yaw);
    }
    if (!doc["pitch"].isNull()) {
      JsonObject pitch = doc["pitch"];
      bleRtDriveServo(false, pitch);
    }
  } else if (strcmp(cmd, "rtAvatar") == 0) {
    handleBleRtAvatar(doc);
  } else if (strcmp(cmd, "rtExpression") == 0) {
    if (!bleRtApplyExpression(doc["expression"]))
      M5_LOGD("rtExpression cmd missing/invalid expression");
  } else {
    M5_LOGD("rt unknown cmd: %s", cmd);
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

  pRtChar = pService->createCharacteristic(
      BLEUUID(BLE_CHAR_RT_UUID), BLECharacteristic::PROPERTY_READ |
                                     BLECharacteristic::PROPERTY_WRITE |
                                     BLECharacteristic::PROPERTY_NOTIFY);
  pRtChar->addDescriptor(new BLE2902());
  pRtChar->setCallbacks(new RtCharacteristicCallbacks());

  pBatteryChar = pService->createCharacteristic(
      BLEUUID((uint16_t)BLE_CHAR_BATTERY_UUID),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pBatteryChar->addDescriptor(new BLE2902());
  uint8_t battery_level = 96;
  pBatteryChar->setValue(&battery_level, 1);

  pService->start();

  if (bleCmdQueue == nullptr)
    bleCmdQueue = xQueueCreate(BLE_CMD_QUEUE_DEPTH, sizeof(BleCmdItem));

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
  pRtChar = nullptr;
  pBatteryChar = nullptr;
  motion_assembler.reset();
  avatar_assembler.reset();
  config_assembler.reset();
  rt_assembler.reset();
  if (bleCmdQueue != nullptr) {
    BleCmdItem item;
    while (xQueueReceive(bleCmdQueue, &item, 0) == pdTRUE)
      free(item.json);
    vQueueDelete(bleCmdQueue);
    bleCmdQueue = nullptr;
  }
  BLEDevice::deinit();
  M5_LOGI("BLE server stopped.");
}

void processBLE(void) {
  /* Deferred command execution: runs in the loop task (see queue note above).
   * Battery is a static value readable via the READ property; no periodic
   * notifications are needed. notify() with no connected client can stall the
   * loop, so never push from here. */
  if (bleCmdQueue == nullptr)
    return;
  for (int i = 0; i < BLE_CMD_QUEUE_DEPTH; i++) {
    BleCmdItem item;
    if (xQueueReceive(bleCmdQueue, &item, 0) != pdTRUE)
      break;
    if (item.json != nullptr) {
      switch (item.tag) {
      case BLE_CMD_MOTION:
        handleBleMotionData(item.json, strlen(item.json));
        break;
      case BLE_CMD_AVATAR:
        handleBleAvatarData(item.json, strlen(item.json));
        break;
      case BLE_CMD_CONFIG:
        handleBleConfigData(item.json, strlen(item.json));
        break;
      case BLE_CMD_RT:
        handleBleRtData(item.json, strlen(item.json));
        break;
      default:
        break;
      }
      free(item.json);
    }
  }
}
