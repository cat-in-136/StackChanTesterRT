#include "ble_gatt_cmd.h"

#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <BLEUtils.h>
#include <M5Unified.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "servo_controller.h"

extern ServoController servo_controller;

/* ---- BLE UUIDs (secure revision) ----
 * Old e2e5e5e0 service with e1/e2/e3/e4/ef characteristics is retired.
 * This revision exposes only:
 *   - StackChanRT dedicated command characteristic (...f1)
 *   - Battery characteristic (standard 0x2A19, unchanged/open)
 * Clients must re-discover; old UUIDs are intentionally not served. */
#define BLE_SERVICE_UUID "e2e5e5f0-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_RT_UUID "e2e5e5f1-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_BATTERY_UUID 0x2A19

#define BLE_DEVICE_NAME "StackChanRT"
#define BLE_MAX_JSON_LEN 2048
#define BLE_FRAG_HEADER_LEN 10

/* ---- Avatar speech text limit (tentative) ----
 * Balloon has no wrapping/scrolling, so over-long text overflows the
 * 320px display. Until auto-shrink/wrap is implemented, reject text
 * longer than this (bytes) with an error log and keep the old display. */
#define BLE_SPEECH_TEXT_MAX_LEN 32

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

/* ---- Secure pairing policy ----
 * Dynamic PIN display (DisplayOnly) + Passkey Entry + MITM + SC + Bond.
 * The Bluedroid stack generates a random 6-digit passkey per pairing and
 * reports it via ESP_GAP_BLE_PASSKEY_NOTIF_EVT -> onPassKeyNotify().
 * No static PIN is set, so the displayed PIN is dynamic by construction.
 * The RT characteristic additionally requires link encryption with MITM,
 * so plaintext access before pairing is rejected by the controller. */

/* ---- GATT Characteristic instances ---- */
static BLECharacteristic *pRtChar = nullptr;
static BLECharacteristic *pBatteryChar = nullptr;
static BLEServer *pServer = nullptr;

/* ---- PIN display handoff ----
 * GAP security callbacks run in the BT host task and must not touch the
 * Avatar (suspend/resume races the draw task). They only publish the latest
 * passkey state here; processBLE() (loop task) renders it. */
static volatile uint32_t s_passkey = 0;
static volatile bool s_passkey_pending = false;
static volatile bool s_link_encrypted = false;
static volatile bool s_pairing_ok = false; // set on auth success, consumed by loop
static volatile bool s_clear_display = false; // set on disconnect, consumed by loop
static bool s_pin_shown = false;             // loop-task-local edge state
static bool s_paired_msg = false;            // loop-task-local: success msg showing
static uint32_t s_paired_until_ms = 0;       // loop-task-local: success msg expiry
#define PAIRED_MSG_MS 10000

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

static FragAssembler rt_assembler;

/* ---- Forward declarations ---- */
static void handleBleRtData(const char *json, size_t len);

/* ---- Deferred execution queue ----
 * GATT onWrite runs in the BLE host task, NOT in loop(). Executing Dynamixel
 * calls there (shared HardwareSerial with loop()'s servo_controller.update())
 * or Avatar calls (setExpression suspends/resumes the draw task) races with
 * the loop task and the Avatar draw task, corrupting state and panicking
 * (Double exception). So onWrite only enqueues a copy; processBLE() (called
 * from loop()) does the real work in the loop task. */
enum BleCmdTag : uint8_t {
  BLE_CMD_RT = 0,
};
struct BleCmdItem {
  uint8_t tag;
  char *json; // malloc'ed NUL-terminated copy, freed by processBLE()
};
#define BLE_CMD_QUEUE_DEPTH 8
static QueueHandle_t bleCmdQueue = nullptr;

static void bleEnqueue(uint8_t tag, const uint8_t *data, size_t len) {
  // NOTE: BT task context (called from onWrite via gattServerEventHandler):
  // never use M5_LOG* (display target) here — serial-only logging.
  if (bleCmdQueue == nullptr || data == nullptr || len == 0)
    return;
  if (len > BLE_MAX_JSON_LEN) {
    log_d("ble cmd too long (%u), dropped", (unsigned)len);
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
    // BT task context: never use M5_LOG* (display target) here.
    log_d("ble queue full, dropped tag=%d", tag);
  }
}

/* ---- Characteristic callbacks (enqueue only; see queue note above) ----
 * BT task context: never use M5_LOG* (display target) here. */
/* StackChanRT dedicated characteristic (...f1);
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

/* ---- BLE security callbacks (BT task context: publish state only) ----
 * BT task context (invoked from BLEDevice::gapEventHandler): never use
 * M5_LOG* (display target) here — use serial-only log_* to avoid display
 * SPI transactions racing the Avatar drawLoop task. */
class BleSecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    // DisplayOnly side never inputs a key; this should not fire.
    return 0;
  }
  void onPassKeyNotify(uint32_t pass_key) override {
    s_passkey = pass_key % 1000000;
    s_passkey_pending = true;
    s_link_encrypted = false;
    s_pairing_ok = false;
    log_i("BLE passkey: %06u (enter on peer)", (unsigned)s_passkey);
  }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    if (cmpl.success) {
      s_link_encrypted = true;
      s_passkey_pending = false;
      s_pairing_ok = true;
      log_i("BLE pairing success, link encrypted");
    } else {
      s_link_encrypted = false;
      s_passkey_pending = false;
      s_pairing_ok = false;
      log_i("BLE pairing failed: reason=0x%x", cmpl.fail_reason);
    }
  }
  bool onConfirmPIN(uint32_t pin) override {
    (void)pin;
    // Numeric-comparison path is not used for DisplayOnly Passkey Entry,
    // but accept if it ever fires.
    return true;
  }
};

static BleSecurityCallbacks bleSecurityCallbacks;

/* ---- Handle BLE Avatar weight ----
 * StackChan spec weight 0..100 -> m5avatar open ratio 0.0..1.0. */
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

/* ---- Handle StackChanRT Data (...f1) ----
 * All StackChanRT commands live here.
 *   {"cmd":"rtMotion","yaw":{angle,speed},"pitch":{...}} (M5 units)
 *   {"cmd":"rtAvatar","leftEye":{x,y,rotation,weight,size},...}
 *   {"cmd":"rtExpression","expression":"Happy"|0..5}
 *   {"cmd":"rtSpeechText","text":"Hello"} ("speechText" alias accepted)
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

/* rtSpeechText value: {"cmd":"rtSpeechText","text":"..."}.
 * "speechText" is accepted as an alias for "text". Empty string clears
 * the balloon. Over-long text is rejected with an error log and the
 * display is left unchanged (Balloon has no wrapping).
 * NOTE: runs in the loop task via processBLE(), same as rtExpression. */
static bool bleRtApplySpeechText(JsonVariant v) {
  if (v.isNull())
    return false;
  if (!v.is<const char *>() && !v.is<String>())
    return false;
  const char *text = v.as<const char *>();
  if (text == nullptr)
    return false;
  size_t len = strlen(text);
  if (len > BLE_SPEECH_TEXT_MAX_LEN) {
    M5_LOGE("rtSpeechText too long (%u > %u), ignored",
            (unsigned)len, (unsigned)BLE_SPEECH_TEXT_MAX_LEN);
    return false;
  }
  if (s_passkey_pending) {
    // Keep the pairing PIN visible; user text must not hide it.
    M5_LOGD("rtSpeechText ignored while PIN prompt active");
    return false;
  }
  avatar.setSpeechText(text);
  // User text takes over from the pairing-success message: cancel its
  // auto-clear timer so the message expiry does not wipe the new text.
  s_paired_msg = false;
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
  } else if (strcmp(cmd, "rtSpeechText") == 0) {
    JsonVariant t = doc["text"];
    if (t.isNull())
      t = doc["speechText"];
    if (!bleRtApplySpeechText(t))
      M5_LOGD("rtSpeechText cmd missing/invalid text");
  } else {
    M5_LOGD("rt unknown cmd: %s", cmd);
  }
}

/* ---- PIN display pump (loop task only) ----
 * Priority: passkey prompt > pairing-success message (10 s) > blank.
 * NOTE: Avatar speech text is ASCII-only here (PIN digits / "Pairing OK"). */
static void pollPinDisplay(void) {
  if (s_clear_display) {
    s_clear_display = false;
    s_pairing_ok = false;
    avatar.setSpeechText("");
    s_pin_shown = false;
    s_paired_msg = false;
    return;
  }
  if (s_passkey_pending) {
    s_paired_msg = false; // PIN prompt takes precedence over success message
    if (!s_pin_shown) {
      char buf[16];
      snprintf(buf, sizeof(buf), "PIN:%06u", (unsigned)(s_passkey % 1000000));
      avatar.setSpeechText(buf);
      s_pin_shown = true;
    }
    return;
  }
  if (s_pin_shown) {
    avatar.setSpeechText("");
    s_pin_shown = false;
  }
  if (s_pairing_ok) {
    s_pairing_ok = false; // consume
    avatar.setSpeechText("Pairing OK");
    s_paired_msg = true;
    s_paired_until_ms = millis() + PAIRED_MSG_MS;
  } else if (s_paired_msg &&
             (int32_t)(millis() - s_paired_until_ms) >= 0) {
    avatar.setSpeechText("");
    s_paired_msg = false;
  }
}

/* ---- BLE Setup ---- */
// NOTE: MyServerCallbacks runs in BT task context (gattServerEventHandler):
// never use M5_LOG* (display target) here — use serial-only log_*.
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override { (void)pServer; }
  void onDisconnect(BLEServer *pServer) override {
    s_passkey_pending = false;
    s_link_encrypted = false;
    s_pairing_ok = false;
    s_clear_display = true; // loop task clears any PIN/success message
    pServer->startAdvertising();
  }
};

void setupBLE(void) {
  M5_LOGI("Starting BLE server...");

  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(247);

  /* Secure Connection + MITM + Bond with DisplayOnly I/O.
   * No static PIN: the stack generates a dynamic 6-digit passkey per
   * pairing and delivers it to onPassKeyNotify() for display. */
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(&bleSecurityCallbacks);
  BLESecurity security;
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security.setCapability(ESP_IO_CAP_OUT);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setKeySize(16);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(BLEUUID(BLE_SERVICE_UUID));

  pRtChar = pService->createCharacteristic(
      BLEUUID(BLE_CHAR_RT_UUID), BLECharacteristic::PROPERTY_READ |
                                     BLECharacteristic::PROPERTY_WRITE |
                                     BLECharacteristic::PROPERTY_NOTIFY);
  pRtChar->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM |
                                ESP_GATT_PERM_WRITE_ENC_MITM);
  pRtChar->addDescriptor(new BLE2902());
  pRtChar->setCallbacks(new RtCharacteristicCallbacks());

  /* Battery stays open (unchanged): readable without pairing. */
  pBatteryChar = pService->createCharacteristic(
      BLEUUID((uint16_t)BLE_CHAR_BATTERY_UUID),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pBatteryChar->addDescriptor(new BLE2902());
  uint8_t battery_level = 96;
  pBatteryChar->setValue(&battery_level, 1);

  pService->start();

  if (bleCmdQueue == nullptr)
    bleCmdQueue = xQueueCreate(BLE_CMD_QUEUE_DEPTH, sizeof(BleCmdItem));

  s_passkey = 0;
  s_passkey_pending = false;
  s_link_encrypted = false;
  s_pairing_ok = false;
  s_clear_display = false;
  s_pin_shown = false;
  s_paired_msg = false;
  s_paired_until_ms = 0;

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLEUUID(BLE_SERVICE_UUID));
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  M5_LOGI("BLE server started. Waiting for connections...");
}

void teardownBLE(void) {
  // Loop-task context: safe to clear Avatar speech directly.
  avatar.setSpeechText("");
  s_passkey_pending = false;
  s_link_encrypted = false;
  s_pairing_ok = false;
  s_clear_display = false;
  s_pin_shown = false;
  s_paired_msg = false;
  s_paired_until_ms = 0;
  if (pServer != nullptr) {
    BLEServer *srv = pServer;
    pServer = nullptr;
    delete srv;
  }
  pRtChar = nullptr;
  pBatteryChar = nullptr;
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
  pollPinDisplay();
  if (bleCmdQueue == nullptr)
    return;
  for (int i = 0; i < BLE_CMD_QUEUE_DEPTH; i++) {
    BleCmdItem item;
    if (xQueueReceive(bleCmdQueue, &item, 0) != pdTRUE)
      break;
    if (item.json != nullptr) {
      switch (item.tag) {
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
