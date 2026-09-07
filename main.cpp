#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ctype.h>
#include <string.h>
#include <SPIFFS.h>

// Channel-hop modes are needed by board_config.h (per-board CHANNEL_MODE).
#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

// Per-board pins / dimensions / touch controller (ADR-2).
#include "board_config.h"

#if FY_UI_BUILD
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <TinyGPSPlus.h>
#if FY_TOUCH_FT6236_I2C
#include <Wire.h>
#endif
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#endif

// ============================================================
// CONFIG
// ============================================================

// Wire contract with the shipped deflock-app companion: the BLE
// advertisement name and protocol version are compatibility constants,
// identical on every board. Do not change without a coordinated app update.
#define CYD_PROTOCOL_VERSION  1
#define CYD_PAIR_NAME         "CYD-Flock-You"
#define CYD_GPS_STALE_MS      10000
#define CYD_UI_REFRESH_MS     1000

#if FY_UI_BUILD
// Colors and BLE/scan constants shared by every UI board.
#define CYD_BLE_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CYD_BLE_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CYD_BLE_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CYD_BLE_CHUNK_BYTES   20

// ── BLE Flock Battery Detection ──────────────────────────────
// Flock Safety cameras with external batteries use Bluetooth to relay
// battery health data to the camera. The BLE advertising signatures are:
//   - Device name: "Penguin-NNNNNNNNNN" (10 digits) [older firmware]
//   - Device name: "NNNNNNNNNN" (10 digits) [post March 2025 firmware]
//   - Device name: "FS Ext Battery"
//   - Manufacturer Specific advertising data with Company ID 0x09C8 (XUNTONG)
// Research: Ryan O'Horo (ryanohoro.com) + ESP32 Marauder (justcallmekoko)
#define CYD_BLE_FLOCK_SCAN_INTERVAL_MS  16000   // scan every 16s (near-continuous)
#define CYD_BLE_FLOCK_SCAN_DURATION_MS  15000   // scan for 15s (94% duty cycle, USB powered)
#define CYD_BLE_FLOCK_RSSI_MIN          -100
#define CYD_BLE_FLOCK_MAX_DETECTIONS    50
#define XUNTONG_COMPANY_ID             0x09C8

// RGB565 approximations of the FlockFree app palette.
#define CYD_COLOR_BG          0x0004
#define CYD_COLOR_NAVY        0x0043
#define CYD_COLOR_SURFACE     0x0829
#define CYD_COLOR_SURFACE_2   0x104E
#define CYD_COLOR_CYAN        0x06FC
#define CYD_COLOR_BLUE        0x03B9
#define CYD_COLOR_DANGER      0xC0E5
#define CYD_COLOR_GREEN       0x35C9
#define CYD_COLOR_AMBER       0xFD20
#define CYD_COLOR_TEXT        0xFFFF
#define CYD_COLOR_MUTED       0x9CD3
#define CYD_COLOR_LINE        0x067A
#endif // FY_UI_BUILD

// Dynamic screen dimensions, updated when rotation changes.
#if FY_UI_BUILD
static uint16_t cydScreenW = CYD_TFT_W;
static uint16_t cydScreenH = CYD_TFT_H;
#endif

#if !defined(USE_LED)
// Onboard user LED on Seeed XIAO ESP32-S3 is GPIO21 and is ACTIVE LOW
// (driving the pin LOW lights the LED).
#define LED_PIN          21
#define USE_LED          1
#define LED_ACTIVE_HIGH  0
#define LED_FLASH_MS     120
#endif
// (CYD and CrowPanel LED settings come from board_config.h; on boards with
// no user LED, USE_LED is 0 and LED_PIN stays undefined.)

#if !defined(MIRROR_SERIAL)
#define MIRROR_SERIAL    1
#define MIRROR_TX_PIN    43
#define MIRROR_BAUD      115200
#endif

#if !defined(CHANNEL_MODE)
#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
#define CHANNEL_DWELL_MS 250
#endif
#define SINGLE_CHANNEL 1

static const uint8_t customChannels[]  = {11, 6, 1};
static const size_t  customChannelCount = sizeof(customChannels) / sizeof(customChannels[0]);

// Flock channel-priority hop: hits the three non-overlapping 2.4 GHz
// channels (1, 6, 11) first, then fills in the rest. Doubles interception
// probability for cameras on common channels.
static const uint8_t fullHopChannels[] = {11,6,1,10,5,2,9,4,3,8,7,12,13};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

#define HEARTBEAT_MS    30000
#define RSSI_MIN        -100
#define ALERT_COOLDOWN_MS 5000

// Audio cadence: two fast ascending beeps on a NEW MAC, then while any
// target is still in range (seen within HB_DEVICE_ACTIVE_MS), two monotone
// heartbeat beeps every HB_BEEP_INTERVAL_MS.
#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
// A MAC we haven't heard from in REDISCOVER_MS counts as a fresh discovery
// next time it shows up — fires the ascending chirp again. Shorter than a
// Flock's burst-sleep gap would mean false chirps; longer means you'd miss
// a drive-away/return. 30 s is a good middle ground.
#define REDISCOVER_MS          30000
#define NEW_CHIRP_LO_HZ        2000
#define NEW_CHIRP_HI_HZ        2800
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             1500
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

#define ENABLE_SSID_MATCH 1
#define CHECK_ADDR1 1   // dst/rx — catches Flock STAs receiving probe responses
#define CHECK_ADDR3 0   // bssid fallback for randomised addr2
static const char* target_ssid_keywords[] = { "flock", "flck", "test_flck" };
static const size_t SSID_KEYWORD_COUNT = sizeof(target_ssid_keywords) / sizeof(target_ssid_keywords[0]);

#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

// Persistence
#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define FY_ROTATION_FILE     "/rotation"
#define FY_ROTATION_TMP      "/rotation.tmp"
#define AUTOSAVE_INTERVAL_MS 60000

// ============================================================
// TARGET OUI LIST  (all lowercase, colons only)
// ============================================================

static const char* target_ouis[] = {
  "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
  "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
  "94:08:53", "e4:aa:ea", "f4:6a:dd", "f8:a2:d6", "24:b2:b9",
  "00:f4:8d", "d0:39:57", "e8:d0:fc", "e0:4f:43", "b8:1e:a4",
  "70:08:94", "58:8e:81", "ec:1b:bd", "3c:71:bf", "58:00:e3",
  "90:35:ea", "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12",
  // Contributed by Michael / DeFlockJoplin — discovered via wildcard-probe
  // + OUI signature during field testing. The 12th camera in his drive-test
  // used this prefix and wasn't in @NitekryDPaul's original 30.
  "82:6b:f2",
  // Flock Safety direct OUI — IEEE registered to Flock Safety Inc.
  // Source: IEEE OUI registry, upstream issue #28 / PR #29
  "b4:1e:52",
  // Liteon Technology — reported in upstream issue #28 / PR #29
  "e0:0a:f6"
};
// Locally-administered OUI prefixes that we explicitly allowlist.
// These bypass the LAA bit guard in matchOuiRaw() because they are
// known Flock camera signatures even though they use randomised MAC space.
static const char* target_ouis_laa[] = {
  "82:6b:f2"   // DeFlockJoplin field-confirmed, LAA prefix
};
static const size_t OUI_LAA_COUNT = sizeof(target_ouis_laa) / sizeof(target_ouis_laa[0]);
static uint8_t oui_laa_bytes[OUI_LAA_COUNT][3];

static const size_t OUI_COUNT = sizeof(target_ouis) / sizeof(target_ouis[0]);

// Pre-compiled byte table — populated once in setup(), never touched again.
// Keeps matchOuiRaw entirely in IRAM with no flash-resident function calls.
static uint8_t oui_bytes[OUI_COUNT][3];

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 32

typedef enum : uint8_t {
  ALERT_OUI_ADDR2       = 0,
  ALERT_OUI_ADDR1       = 1,
  ALERT_OUI_ADDR3       = 2,
  ALERT_SSID            = 3,
  // Probe Request + wildcard SSID (tag 0, length 0) from a known-OUI addr2.
  // Tight signature from Michael / DeFlockJoplin field research:
  //   https://github.com/DeflockJoplin/flock-you
  ALERT_WILDCARD_PROBE  = 4,
  ALERT_HIDDEN_SSID     = 5,
  ALERT_WILDCARD_PROBE_IE_SIG = 6,  // OUI + wildcard SSID + full IE fingerprint
} AlertType;

// Per-method hit counters (indexed by AlertType)
static uint32_t methodCounts[8] = {0};

typedef struct {
  AlertType type;
  uint8_t   mac[6];
  int8_t    rssi;
  uint8_t   channel;
  char      ssid[33];     // populated for SSID hits
  char      frameKind[12];
} AlertEntry;

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
static volatile size_t alertHead = 0;  // written by callback
static volatile size_t alertTail = 0;  // read by loop()
static portMUX_TYPE    queueMux  = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t wifiRxFrames = 0;
static volatile uint32_t wifiRxMgmtFrames = 0;
static volatile uint32_t wifiRxDataFrames = 0;
static volatile uint32_t alertQueueDrops = 0;

static void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac, int8_t rssi,
                                    uint8_t ch, const char* ssid, const char* kind) {
  portENTER_CRITICAL_ISR(&queueMux);
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {                         // drop if full — loop() is behind
    alertQueueDrops++;
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  memcpy((void*)e->mac, mac, 6);

  if (ssid)  { strncpy((char*)e->ssid,      ssid, 32); ((char*)e->ssid)[32] = '\0'; }
  else        { ((char*)e->ssid)[0] = '\0'; }

  if (kind)  { strncpy((char*)e->frameKind, kind, 11); ((char*)e->frameKind)[11] = '\0'; }
  else        { ((char*)e->frameKind)[0] = '\0'; }

  alertHead = next;
  portEXIT_CRITICAL_ISR(&queueMux);
}

// ============================================================
// DETECTION TABLE  (on-device storage, persisted to SPIFFS)
// ============================================================
//
// Single-threaded: only touched from loop() — drainAlertQueue() adds, and
// fySaveSession() reads. No mutex needed. The WiFi-task callback never
// touches this table; it only writes to the lock-free alert ring buffer.

typedef struct {
  char     mac[18];
  char     method[16];     // "oui_addr2" / "oui_addr1" / "hidden_ssid" / etc.
  int8_t   rssi;
  uint8_t  channel;
  uint32_t firstSeen;      // millis() at first hit
  uint32_t lastSeen;       // millis() at latest hit
  uint16_t count;
  char     ssid[33];       // "" unless an SSID hit populated it
} FYDetection;

static FYDetection fyDet[MAX_DETECTIONS];
static int           fyDetCount       = 0;
static bool          fySpiffsReady    = false;
static bool          fyDirty          = false;
static unsigned long fyLastSaveAt     = 0;
static int           fyLastSaveCount  = 0;

// ============================================================
// STATE
// ============================================================

static uint8_t  currentChannel = 1;
static size_t   customChannelIndex = 0;
static size_t   fullHopIndex = 0;
static unsigned long lastHop = 0;
static unsigned long lastHeartbeat = 0;
static volatile bool sniffingStopped = false;

// Dedupe table (small circular, avoids single-slot eviction bug).
// This is the *serial-rate-limit* dedup — it suppresses beep + emit within
// ALERT_COOLDOWN_MS of a prior hit on the same MAC. The detection table
// (above) still counts every hit regardless of this suppression.
#define DEDUPE_SLOTS 16
static struct {
  char mac[18];
  unsigned long ts;
} dedupeTable[DEDUPE_SLOTS];
static size_t dedupeIdx = 0;

// LED one-shot pulse timer
static volatile unsigned long ledOffAt = 0;

// Heartbeat audio state: last time any target was seen, last time the
// heartbeat beep-pair was played. When nothing has been seen for
// HB_DEVICE_ACTIVE_MS the heartbeat stops until the next new detection.
static unsigned long fyLastTargetSeen  = 0;
static unsigned long fyLastHeartbeatAt = 0;

#if FY_UI_BUILD
// ============================================================
// CYD DISPLAY / GPS / SD STATE
// ============================================================

static TFT_eSPI tft = TFT_eSPI();
static TinyGPSPlus phoneGps;

typedef enum : uint8_t {
  SCREEN_SCAN = 0,
  SCREEN_GPS,
  SCREEN_LOG,
  SCREEN_LAST,
  SCREEN_COUNT
} CydScreen;

typedef struct {
  bool hasFix;
  bool hasTime;
  double lat;
  double lng;
  double accuracyM;
  double speedKmph;
  double courseDeg;
  uint32_t sats;
  double hdop;
  uint32_t unixTime;
  int16_t utcOffsetMin;
  unsigned long lastFixMs;
  unsigned long lastTimeSyncMs;
  char source[8];
} CydGpsState;

static CydGpsState cydGps = {};
static CydScreen cydScreen = SCREEN_SCAN;
static CydScreen cydLastDrawnScreen = SCREEN_COUNT;
static bool cydDisplayReady = false;
static bool cydSdReady = false;
static uint32_t cydCsvRows = 0;
static uint32_t cydSdFailures = 0;
static unsigned long cydLastUiDraw = 0;
static unsigned long cydLastButtonAt = 0;
static bool cydLastButtonState = HIGH;
static uint8_t cydTftRotation = CYD_TFT_ROTATION;
static unsigned long cydLastTouchMs = 0;
static bool cydLastTouchDown = false;
static char cydLastMac[18] = "";
static char cydLastMethod[20] = "";
static int cydLastRssi = 0;
static uint8_t cydLastChannel = 0;
static uint16_t cydLastCount = 0;
static bool cydLastLocationValid = false;
static double cydLastLat = 0;
static double cydLastLng = 0;
static unsigned long cydLastDetectionMs = 0;
static unsigned long cydLastPairAnnounce = 0;

// Full-screen Flock Found flash overlay
#define CYD_FLASH_DURATION_MS 10000
static unsigned long cydFlashStartMs = 0;
static bool cydFlashActive = false;

static BLECharacteristic* cydBleTx = nullptr;
static bool cydBleReady = false;
static volatile bool cydBleClientConnected = false;
static volatile bool cydBleConnectionChanged = false;

#define CYD_BLE_RX_QUEUE_SIZE 8
#define CYD_BLE_RX_LINE_SIZE 180
static char cydBleRxQueue[CYD_BLE_RX_QUEUE_SIZE][CYD_BLE_RX_LINE_SIZE];
static volatile uint8_t cydBleRxHead = 0;
static volatile uint8_t cydBleRxTail = 0;
static portMUX_TYPE cydBleRxMux = portMUX_INITIALIZER_UNLOCKED;

// BLE Flock battery detection state
typedef struct {
  char     mac[18];
  char     name[32];
  int8_t   rssi;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint16_t count;
  bool     isXuntong;
} BleFlockDetection;

static BleFlockDetection bleFlockDetections[CYD_BLE_FLOCK_MAX_DETECTIONS];
static int bleFlockDetCount = 0;
static unsigned long bleFlockLastScanMs = 0;
static bool bleFlockScanning = false;
static unsigned long bleFlockScanStartMs = 0;
static void cydBleWriteBytes(const uint8_t* data, size_t len);
static void cydSetDisplayRotation(uint8_t rotation, bool redraw);
#endif
static void emitDetectionJSON(const char* mac, const char* method,
                              int8_t rssi, uint8_t ch, const char* ssid,
                              const char* confidence = nullptr);

// ============================================================
// 802.11 HEADER
// ============================================================

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// ============================================================
// HELPERS
// ============================================================

// Dual-output: prints to both Serial (USB) and Serial1 (GPIO43)
static char _dualBuf[1024];

static void dualPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dualPrintf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(_dualBuf, sizeof(_dualBuf), fmt, args);
  va_end(args);
  if (n > 0) {
    size_t written = (size_t)n;
    if (written >= sizeof(_dualBuf)) {
      written = sizeof(_dualBuf) - 1;
    }
    Serial.write(_dualBuf, written);
#if MIRROR_SERIAL
    Serial1.write(_dualBuf, written);
#endif
#if FY_UI_BUILD
    cydBleWriteBytes((const uint8_t*)_dualBuf, written);
#endif
  }
}

static void dualPrintln(const char* str) {
  Serial.println(str);
#if MIRROR_SERIAL
  Serial1.println(str);
#endif
#if FY_UI_BUILD
  cydBleWriteBytes((const uint8_t*)str, strlen(str));
  cydBleWriteBytes((const uint8_t*)"\n", 1);
#endif
}

static inline void ledSet(bool on) {
#if USE_LED
#if LED_ACTIVE_HIGH
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LED_PIN, on ? LOW  : HIGH);
#endif
#endif
}

static void ledFlash(unsigned ms) {
#if USE_LED
  ledSet(true);
  ledOffAt = millis() + ms;
  if (ledOffAt == 0) ledOffAt = 1;  // avoid the "off" sentinel
#endif
}

static void ledTick() {
#if USE_LED
  if (ledOffAt && (long)(millis() - ledOffAt) >= 0) {
    ledSet(false);
    ledOffAt = 0;
  }
#endif
}

static void buzzerBeep(unsigned int ms) {
#if USE_BUZZER
  digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
#endif
}

// Two fast ascending beeps — played on the FIRST sighting of a MAC.
static void newDetectChirp() {
#if USE_BUZZER
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
  delay(NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}

// Two monotone beeps — periodic heartbeat while at least one target is still
// in range (last seen within HB_DEVICE_ACTIVE_MS).
static void heartbeatBeep() {
#if USE_BUZZER
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
  delay(HB_BEEP_GAP_MS);
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}
static void startupBeep() {
#if USE_BUZZER
  // First 6 notes of SMB World 1-2 (underground). Koji Kondo's descending
  // pattern: C5 → C4 → A4 → A3 → G#4 → G#3 (alternating-octave pairs).
  static const uint16_t notes[6] = { 523, 262, 440, 220, 415, 208 };
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay((i == 5) ? 160 : 95);
    noTone(BUZZER_PIN);
    if (i < 5) delay(22);
  }
#endif
}

static void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void ouiFromMac(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

static void precompileOuis() {
  for (size_t i = 0; i < OUI_COUNT; i++) {
    const char* o  = target_ouis[i];
    oui_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
  for (size_t i = 0; i < OUI_LAA_COUNT; i++) {
    const char* o  = target_ouis_laa[i];
    oui_laa_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_laa_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_laa_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
}

// Bit 0 of byte 0 set = multicast/broadcast — never a real device transmitter or receiver
// we care about. Guards addr1 checks against 01:xx, 33:33:xx, ff:ff:ff:ff:ff:ff etc.
static inline bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

static bool IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  // Locally-administered (randomised) MACs have bit 1 of byte 0 set.
  // Fixed infrastructure devices never use them — skip unless the prefix is
  // explicitly in our LAA allowlist (e.g. 82:6b:f2 from DeFlockJoplin).
  if (mac[0] & 0x02) {
    for (size_t i = 0; i < OUI_LAA_COUNT; i++) {
      if (mac[0] == oui_laa_bytes[i][0] &&
          mac[1] == oui_laa_bytes[i][1] &&
          mac[2] == oui_laa_bytes[i][2]) return true;
    }
    return false;
  }

  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (mac[0] == oui_bytes[i][0] &&
        mac[1] == oui_bytes[i][1] &&
        mac[2] == oui_bytes[i][2]) return true;
  }
  return false;
}

static char* strcasestr_local(const char* haystack, const char* needle) {
  if (!*needle) return (char*)haystack;
  for (; *haystack; ++haystack) {
    const char* h = haystack; const char* n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
    if (!*n) return (char*)haystack;
  }
  return nullptr;
}
static bool matchSsidKeyword(const char* ssid) {
  for (size_t i = 0; i < SSID_KEYWORD_COUNT; i++)
    if (strcasestr_local(ssid, target_ssid_keywords[i])) return true;
  return false;
}

static const char* channelModeName() {
  switch (CHANNEL_MODE) {
    case CHANNEL_MODE_FULL_HOP: return "FULL_HOP";
    case CHANNEL_MODE_CUSTOM:   return "CUSTOM";
    case CHANNEL_MODE_SINGLE:   return "SINGLE";
    default:                    return "UNKNOWN";
  }
}

static inline uint16_t channelFreqMhz(uint8_t ch) {
  return (ch >= 1 && ch <= 14) ? (uint16_t)(2407 + 5 * ch) : 0;
}

static bool shouldSuppressDuplicate(const char* macStr) {
  unsigned long now = millis();
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (strcmp(dedupeTable[i].mac, macStr) == 0) {
      if ((now - dedupeTable[i].ts) < ALERT_COOLDOWN_MS) return true;
      dedupeTable[i].ts = now;
      return false;
    }
  }
  // Not found — insert into next slot
  strlcpy(dedupeTable[dedupeIdx].mac, macStr, 18);
  dedupeTable[dedupeIdx].ts = now;
  dedupeIdx = (dedupeIdx + 1) % DEDUPE_SLOTS;
  return false;
}

static void stopSniffing(const char* reason) {
  if (sniffingStopped) return;
  sniffingStopped = true;
  esp_wifi_set_promiscuous(false);
  dualPrintf("[flockyou] sniffing stopped: %s\n", reason);
}

static void applyInitialChannel() {
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  currentChannel = SINGLE_CHANNEL;
#elif CHANNEL_MODE == CHANNEL_MODE_CUSTOM
  currentChannel = customChannels[0];
#else
  currentChannel = fullHopChannels[0];
#endif
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();  // start dwell timer precisely when channel is first set
}

static void updateChannelMode() {
  if (sniffingStopped) return;
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  if (currentChannel != SINGLE_CHANNEL) {
    currentChannel = SINGLE_CHANNEL;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  }
  return;
#else
  if (millis() - lastHop < CHANNEL_DWELL_MS) return;
  #if CHANNEL_MODE == CHANNEL_MODE_CUSTOM
    customChannelIndex = (customChannelIndex + 1) % customChannelCount;
    currentChannel = customChannels[customChannelIndex];
  #else
    fullHopIndex = (fullHopIndex + 1) % fullHopChannelCount;
    currentChannel = fullHopChannels[fullHopIndex];
  #endif
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
#endif
}

static void printHeartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    dualPrintf("[flockyou] scanning (ch=%u mode=%s det=%d)\n",
                  currentChannel, channelModeName(), fyDetCount);
    lastHeartbeat = millis();
  }
}

// ============================================================
// DETECTION TABLE OPS
// ============================================================

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_addr2";
    case ALERT_OUI_ADDR1:      return "oui_addr1";
    case ALERT_OUI_ADDR3:      return "oui_addr3";
    case ALERT_SSID:           return "ssid";
    case ALERT_WILDCARD_PROBE: return "wildcard_probe";
    case ALERT_WILDCARD_PROBE_IE_SIG: return "wildcard_probe_ie_sig";
    case ALERT_HIDDEN_SSID:    return "hidden_ssid";
    default:                   return "unknown";
  }
}

// Returns index of entry (new or updated), or -1 if table is full.
// Returns index, and sets *outChirpWorthy = true when the caller should fire
// the ascending new-discovery chirp. Chirp-worthy means either (a) MAC is
// brand new to this session, or (b) MAC is known but hasn't been seen in
// REDISCOVER_MS — i.e. it left RF range and came back.
static int fyAddDetection(const char* mac, const char* method,
                          int8_t rssi, uint8_t ch, const char* ssid,
                          bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      if (ssid && ssid[0] && !fyDet[i].ssid[0]) {
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      }
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    if (outChirpWorthy) *outChirpWorthy = false;
    return -1;
  }
  FYDetection& d = fyDet[fyDetCount];
  strlcpy(d.mac,    mac,                       sizeof(d.mac));
  strlcpy(d.method, method ? method : "",      sizeof(d.method));
  d.rssi      = rssi;
  d.channel   = ch;
  d.firstSeen = now;
  d.lastSeen  = now;
  d.count     = 1;
  if (ssid && ssid[0]) strlcpy(d.ssid, ssid, sizeof(d.ssid));
  else                 d.ssid[0] = '\0';
  fyDetCount++;
  fyDirty = true;
  if (outChirpWorthy) *outChirpWorthy = true;
  return fyDetCount - 1;
}

// ============================================================
// JSON ESCAPE  — only needed for SSIDs (user-controlled bytes)
// ============================================================

static size_t jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; src[i]; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      if (o + 2 >= cap) break;
      dst[o++] = '\\'; dst[o++] = c;
    } else if ((unsigned char)c < 0x20) {
      if (o + 6 >= cap) break;
      int n = snprintf(dst + o, cap - o, "\\u%04x", (unsigned)(unsigned char)c);
      if (n <= 0 || (size_t)n >= cap - o) break;
      o += (size_t)n;
    } else {
      if (o + 1 >= cap) break;
      dst[o++] = c;
    }
  }
  dst[o] = '\0';
  return o;
}

// ============================================================
// CRC32  (zlib / SPIFFS-tool compatible polynomial 0xEDB88320)
// ============================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
  }
  return ~crc;
}

// ============================================================
// SPIFFS SESSION PERSISTENCE  — bulletproof envelope format
// ============================================================
//
// Wire format on disk:
//   Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},...]     (exactly B bytes, CRC32 == X)
//
// Atomic write procedure:
//   1. Compute payload size + CRC (pass 1)
//   2. Write envelope + payload to /session.tmp (pass 2)
//   3. Re-validate /session.tmp from disk
//   4. Remove /session.json, rename tmp → main (with copy+delete fallback)
//
// Boot-time recovery:
//   - Try /session.json. If missing or CRC-invalid, try /session.tmp.
//   - Copy whichever validates to /prev_session.json, then delete both.

static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
  char ssidEsc[sizeof(d.ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), d.ssid);
  int n = snprintf(dst, cap,
      "{\"mac\":\"%s\",\"method\":\"%s\",\"rssi\":%d,\"channel\":%u,"
      "\"first\":%lu,\"last\":%lu,\"count\":%u,\"ssid\":\"%s\"}",
      d.mac, d.method, d.rssi, (unsigned)d.channel,
      (unsigned long)d.firstSeen, (unsigned long)d.lastSeen, (unsigned)d.count,
      ssidEsc);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static uint32_t fyComputePayloadCRC(size_t& outBytes) {
  char line[384];
  uint32_t crc = 0;
  outBytes = 0;
  crc = fyCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { crc = fyCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    crc = fyCRC32Update(crc, (const uint8_t*)line, n);
    outBytes += n;
  }
  crc = fyCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
  return crc;
}

// Minimal envelope parser: pulls bytes + crc fields by substring search.
// Robust to field reordering; rejects anything without both required keys.
static bool fyParseEnvelope(const char* hdr, size_t& outBytes, uint32_t& outCrc) {
  const char* b = strstr(hdr, "\"bytes\":");
  const char* c = strstr(hdr, "\"crc\":\"0x");
  if (!b || !c) return false;
  b += 8;
  long long bv = 0;
  if (sscanf(b, "%lld", &bv) != 1 || bv < 0) return false;
  c += 9;
  unsigned cv = 0;
  if (sscanf(c, "%x", &cv) != 1) return false;
  outBytes = (size_t)bv;
  outCrc   = (uint32_t)cv;
  return true;
}

static bool fyValidateSessionFile(const char* path) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;

  String hdr = f.readStringUntil('\n');
  if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }

  size_t   expectedBytes = 0;
  uint32_t expectedCRC   = 0;
  if (!fyParseEnvelope(hdr.c_str(), expectedBytes, expectedCRC)) {
    f.close(); return false;
  }

  size_t bodyOffset = hdr.length() + 1;
  size_t fileSize   = f.size();
  if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
  if ((fileSize - bodyOffset) != expectedBytes) { f.close(); return false; }

  uint8_t buf[256];
  uint32_t crc = 0;
  size_t remaining = expectedBytes;
  while (remaining > 0) {
    int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) break;
    crc = fyCRC32Update(crc, buf, (size_t)n);
    remaining -= (size_t)n;
  }
  f.close();
  return (remaining == 0 && crc == expectedCRC);
}

static bool fySpiffsCopy(const char* src, const char* dst) {
  File s = SPIFFS.open(src, "r");
  if (!s) return false;
  File d = SPIFFS.open(dst, "w");
  if (!d) { s.close(); return false; }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while ((n = s.read(buf, sizeof(buf))) > 0) {
    if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
  }
  s.close();
  d.close();
  return ok;
}

static bool fyAtomicPromote(const char* src, const char* dst) {
  if (SPIFFS.rename(src, dst)) return true;
  if (!fySpiffsCopy(src, dst)) return false;
  SPIFFS.remove(src);
  return true;
}

static void fySaveSession() {
  if (!fySpiffsReady) return;
  if (!fyDirty && fyDetCount == fyLastSaveCount) return;

  size_t   payloadBytes = 0;
  uint32_t crc          = fyComputePayloadCRC(payloadBytes);
  int      savedCount   = fyDetCount;

  File f = SPIFFS.open(FY_SESSION_TMP, "w");
  if (!f) {
    dualPrintf("[flockyou] save failed: cannot open %s\n", FY_SESSION_TMP);
    return;
  }
  f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);

  char line[384];
  size_t wrote = 0;
  f.write((uint8_t*)"[", 1); wrote++;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    f.write((uint8_t*)line, n);
    wrote += n;
  }
  f.write((uint8_t*)"]", 1); wrote++;
  f.close();

  if (wrote != payloadBytes) {
    dualPrintf("[flockyou] save WARNING: wrote %u expected %u — aborting\n",
               (unsigned)wrote, (unsigned)payloadBytes);
    return;
  }

  if (!fyValidateSessionFile(FY_SESSION_TMP)) {
    dualPrintf("[flockyou] save verify FAILED — old session preserved\n");
    return;
  }

  SPIFFS.remove(FY_SESSION_FILE);
  if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
    dualPrintf("[flockyou] promote FAILED — data in %s for recovery\n", FY_SESSION_TMP);
    return;
  }

  fyLastSaveAt    = millis();
  fyLastSaveCount = savedCount;
  fyDirty         = false;
  dualPrintf("[flockyou] session saved: %d det, %u bytes, crc=0x%08lX\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

// Promote any valid session file from last boot into /prev_session.json, then
// start this boot with a fresh empty table. Preserves history across power cycles.
static void fyPromotePrevSession() {
  if (!fySpiffsReady) return;

  const char* source = nullptr;
  if      (fyValidateSessionFile(FY_SESSION_FILE)) source = FY_SESSION_FILE;
  else if (fyValidateSessionFile(FY_SESSION_TMP))  source = FY_SESSION_TMP;

  if (!source) {
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
    dualPrintln("[flockyou] no valid prior session to promote");
    return;
  }

  if (!fySpiffsCopy(source, FY_PREV_FILE)) {
    dualPrintf("[flockyou] failed to promote %s → %s\n", source, FY_PREV_FILE);
    return;
  }
  if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
  if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);

  File v = SPIFFS.open(FY_PREV_FILE, "r");
  size_t sz = v ? v.size() : 0;
  if (v) v.close();
  dualPrintf("[flockyou] prior session promoted from %s (%u bytes)\n",
             source, (unsigned)sz);
}

#if FY_UI_BUILD
// ============================================================
// CYD PHONE PAIRING / DISPLAY / SD CSV
// ============================================================

static bool cydGpsFresh() {
  return cydGps.hasFix && (millis() - cydGps.lastFixMs) <= CYD_GPS_STALE_MS;
}

static void cydSetGps(double lat, double lng, double accuracyM, double speedKmph,
                      double courseDeg, uint32_t sats, double hdop,
                      uint32_t unixTime, int16_t utcOffsetMin,
                      const char* source) {
  cydGps.hasFix = true;
  cydGps.lat = lat;
  cydGps.lng = lng;
  cydGps.accuracyM = accuracyM;
  cydGps.speedKmph = speedKmph;
  cydGps.courseDeg = courseDeg;
  cydGps.sats = sats;
  cydGps.hdop = hdop;
  cydGps.lastFixMs = millis();
  if (unixTime > 0) {
    cydGps.hasTime = true;
    cydGps.unixTime = unixTime;
    cydGps.utcOffsetMin = utcOffsetMin;
    cydGps.lastTimeSyncMs = cydGps.lastFixMs;
  }
  strlcpy(cydGps.source, source ? source : "phone", sizeof(cydGps.source));
}

static void cydFormatClock(char* out, size_t cap, unsigned long now) {
  if (!out || cap == 0) return;
  if (!cydGps.hasTime) {
    strlcpy(out, "--:--", cap);
    return;
  }

  int64_t elapsedSec = (int64_t)((now - cydGps.lastTimeSyncMs) / 1000);
  int64_t localSec = (int64_t)cydGps.unixTime + elapsedSec +
                     ((int64_t)cydGps.utcOffsetMin * 60);
  int32_t daySec = (int32_t)(localSec % 86400);
  if (daySec < 0) daySec += 86400;

  snprintf(out, cap, "%02d:%02d", daySec / 3600, (daySec % 3600) / 60);
}

static uint16_t cydBoolColor(bool ok) {
  return ok ? CYD_COLOR_GREEN : CYD_COLOR_AMBER;
}

static void cydDrawPanel(int x, int y, int w, int h) {
  tft.fillRoundRect(x, y, w, h, 6, CYD_COLOR_SURFACE);
  tft.drawRoundRect(x, y, w, h, 6, CYD_COLOR_LINE);
}

static void cydDrawMetricBox(int x, int y, int w, int h,
                             const char* label, const char* value,
                             uint16_t valueColor) {
  cydDrawPanel(x, y, w, h);
  tft.setTextSize(1);
  tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
  tft.drawString(label, x + 8, y + 4);
  tft.setTextSize(2);
  tft.setTextColor(valueColor, CYD_COLOR_SURFACE);
  tft.drawString(value, x + 8, y + 16);
}

static void cydDrawMetricNumberBox(int x, int y, int w, int h,
                                   const char* label, unsigned long value,
                                   uint16_t valueColor) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", value);
  cydDrawMetricBox(x, y, w, h, label, buf, valueColor);
}

static void cydDrawMetricSignedBox(int x, int y, int w, int h,
                                   const char* label, long value,
                                   uint16_t valueColor) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%ld", value);
  cydDrawMetricBox(x, y, w, h, label, buf, valueColor);
}

static void cydDrawFlockFreeMark(int x, int y, int w, int h) {
  tft.fillRoundRect(x, y, w, h, 8, CYD_COLOR_NAVY);
  tft.drawRoundRect(x, y, w, h, 8, CYD_COLOR_CYAN);
  tft.drawFastHLine(x + 8, y + h - 9, w - 16, CYD_COLOR_BLUE);
  tft.setTextSize(2);
  tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("FF", x + w / 2, y + h / 2);
  tft.setTextDatum(TL_DATUM);
}

static void cydDrawDashboardFooter(unsigned long now) {
  char timeBuf[8];
  cydFormatClock(timeBuf, sizeof(timeBuf), now);

  const int footerY = cydScreenH - 39;
  tft.fillRect(0, footerY, cydScreenW, 39, CYD_COLOR_NAVY);
  tft.drawFastHLine(0, footerY, cydScreenW, CYD_COLOR_CYAN);
  tft.setTextSize(1);
  tft.fillRoundRect(8, footerY + 7, 64, 15, 7,
                    cydBleClientConnected ? CYD_COLOR_GREEN : CYD_COLOR_AMBER);
  tft.setTextColor(CYD_COLOR_BG,
                   cydBleClientConnected ? CYD_COLOR_GREEN : CYD_COLOR_AMBER);
  tft.drawString(cydBleClientConnected ? "BT OK" : "BT WAIT", 14, footerY + 10);

  tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_NAVY);
  tft.drawString(timeBuf, cydScreenW - 72, footerY + 10);

  if (cydLastLocationValid) {
    tft.setTextColor(CYD_COLOR_MUTED, CYD_COLOR_NAVY);
    tft.drawString("LAST LOC", 84, footerY + 9);
    tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_NAVY);
    tft.drawFloat(cydLastLat, 4, 84, footerY + 24);
    tft.drawString(",", 138, footerY + 24);
    tft.drawFloat(cydLastLng, 4, 148, footerY + 24);
  } else {
    tft.setTextColor(CYD_COLOR_MUTED, CYD_COLOR_NAVY);
    tft.drawString("LAST LOC: none", 84, footerY + 24);
  }
}

static void cydEmitPairStatus() {
  dualPrintf(
      "{\"event\":\"pair_status\","
      "\"device\":\"%s\","
      "\"protocol_version\":%u,"
      "\"features\":[\"wifi_promisc\",\"phone_gps\",\"sd_csv\",\"tft_status\",\"ble_uart\"],"
      "\"ble_connected\":%s,"
      "\"gps\":%s,"
      "\"sd\":%s,"
      "\"detections\":%d,"
      "\"csv_rows\":%lu,"
      "\"ble_detections\":%d,"
      "\"scan_mode\":\"%s\","
      "\"channel\":%u,"
      "\"rx_frames\":%lu,"
      "\"rx_mgmt\":%lu,"
      "\"rx_data\":%lu,"
      "\"queue_drops\":%lu,"
      "\"method_counts\":{\"oui_addr2\":%lu,\"oui_addr1\":%lu,\"oui_addr3\":%lu,\"ssid\":%lu,\"wildcard_probe\":%lu,\"hidden_ssid\":%lu,\"wildcard_probe_ie_sig\":%lu}}\n",
      CYD_PAIR_NAME,
      (unsigned)CYD_PROTOCOL_VERSION,
      cydBleClientConnected ? "true" : "false",
      cydGpsFresh() ? "true" : "false",
      cydSdReady ? "true" : "false",
      fyDetCount,
      (unsigned long)cydCsvRows,
      bleFlockDetCount,
      channelModeName(),
      (unsigned)currentChannel,
      (unsigned long)wifiRxFrames,
      (unsigned long)wifiRxMgmtFrames,
      (unsigned long)wifiRxDataFrames,
      (unsigned long)alertQueueDrops,
      (unsigned long)methodCounts[ALERT_OUI_ADDR2],
      (unsigned long)methodCounts[ALERT_OUI_ADDR1],
      (unsigned long)methodCounts[ALERT_OUI_ADDR3],
      (unsigned long)methodCounts[ALERT_SSID],
      (unsigned long)methodCounts[ALERT_WILDCARD_PROBE],
      (unsigned long)methodCounts[ALERT_HIDDEN_SSID],
      (unsigned long)methodCounts[ALERT_WILDCARD_PROBE_IE_SIG]);
}

// ============================================================
// TOUCH — board abstraction (ADR-2)
// ============================================================
// Callers only need a debounced "finger down now" edge plus a debug
// status emitter; coordinates are never used for hit-testing, so no
// calibration is required. Each board implements:
//   boardTouchInit()    — set up pins / bus / controller
//   boardTouchPressed() — true while a finger is on the panel
//   boardTouchDebug()   — FYTOUCH status JSON (non-protocol, debug only)

#if FY_TOUCH_XPT2046_BITBANG
// ---- CYD: bit-banged XPT2046 resistive touch (original impl, unchanged) ----

static void boardTouchInit() {
  pinMode(CYD_TOUCH_CS_PIN, OUTPUT);
  pinMode(CYD_TOUCH_CLK_PIN, OUTPUT);
  pinMode(CYD_TOUCH_MOSI_PIN, OUTPUT);
  pinMode(CYD_TOUCH_MISO_PIN, INPUT);
  pinMode(CYD_TOUCH_IRQ_PIN, INPUT_PULLUP);
  digitalWrite(CYD_TOUCH_CS_PIN, HIGH);
  digitalWrite(CYD_TOUCH_CLK_PIN, LOW);
  digitalWrite(CYD_TOUCH_MOSI_PIN, LOW);
}

static uint8_t cydTouchTransfer(uint8_t out) {
  uint8_t in = 0;
  for (int bit = 7; bit >= 0; bit--) {
    digitalWrite(CYD_TOUCH_MOSI_PIN, (out & (1 << bit)) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(CYD_TOUCH_CLK_PIN, HIGH);
    delayMicroseconds(1);
    in = (uint8_t)((in << 1) | (digitalRead(CYD_TOUCH_MISO_PIN) ? 1 : 0));
    digitalWrite(CYD_TOUCH_CLK_PIN, LOW);
    delayMicroseconds(1);
  }
  return in;
}

static uint16_t cydTouchRead12(uint8_t command) {
  digitalWrite(CYD_TOUCH_CS_PIN, LOW);
  delayMicroseconds(2);
  cydTouchTransfer(command);
  uint16_t hi = cydTouchTransfer(0x00);
  uint16_t lo = cydTouchTransfer(0x00);
  digitalWrite(CYD_TOUCH_CS_PIN, HIGH);
  return (uint16_t)(((hi << 8) | lo) >> 3);
}

// Last raw sample, for the debug log only (see boardTouchPressed).
static uint16_t cydTouchLastX = 0, cydTouchLastY = 0, cydTouchLastZ = 0;

static bool boardTouchPressed() {
  // XPT2046 IRQ line is active low while the panel is pressed — this is the
  // exact pressed-test the original cydTouchReadPoint() used (no z
  // threshold; behavior must stay identical on CYD). The x/y/z reads below
  // match the original read sequence so the debug log line is unchanged.
  if (digitalRead(CYD_TOUCH_IRQ_PIN) != LOW) return false;

  uint16_t z = cydTouchRead12(0xB0);  // Z1 pressure sample
  uint16_t x = cydTouchRead12(0xD0);
  uint16_t y = cydTouchRead12(0x90);
  cydTouchLastX = x; cydTouchLastY = y; cydTouchLastZ = z;
  return true;
}

static void boardTouchDebug() {
  uint16_t z1 = cydTouchRead12(0xB0);
  uint16_t z2 = cydTouchRead12(0xC0);
  uint16_t x = cydTouchRead12(0xD0);
  uint16_t y = cydTouchRead12(0x90);
  dualPrintf(
      "{\"event\":\"touch_status\",\"irq\":%d,\"x\":%u,\"y\":%u,\"z1\":%u,\"z2\":%u}\n",
      digitalRead(CYD_TOUCH_IRQ_PIN), (unsigned)x, (unsigned)y, (unsigned)z1, (unsigned)z2);
}

#elif FY_TOUCH_FT6236_I2C
// ---- CrowPanel: FT6236 capacitive touch, Wire poll (no INT pin wired) ----
// Register map and read pattern follow Elecrow's official A-TOUCH demo
// (Elecrow-RD/esp32-terminal): TD_STATUS (0x02) = number of touch points;
// XH/XL/YH/YL = 0x03..0x06. Coordinates are read for debug only.

#define FT6236_REG_TD_STATUS 0x02
#define FT6236_REG_XH        0x03
#define FT6236_REG_XL        0x04
#define FT6236_REG_YH        0x05
#define FT6236_REG_YL        0x06
#define FT6236_REG_VENDID    0xA8
#define FT6236_REG_CHIPID    0xA6

static uint8_t ft6236ReadReg(uint8_t reg) {
  Wire.beginTransmission(FY_TOUCH_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;  // NACK/bus error
  uint8_t n = Wire.requestFrom(FY_TOUCH_I2C_ADDR, (uint8_t)1);
  if (n != 1) return 0xFF;
  return (uint8_t)Wire.read();
}

static void boardTouchInit() {
  Wire.begin(FY_TOUCH_I2C_SDA, FY_TOUCH_I2C_SCL, FY_TOUCH_I2C_FREQ_HZ);
  uint8_t vendor = ft6236ReadReg(FT6236_REG_VENDID);
  uint8_t chip   = ft6236ReadReg(FT6236_REG_CHIPID);
  // FTDI vendor id for FT6xx6 controllers is 0x11; chip id 0x64 = FT6236.
  // A read returning 0xFF means no device — touch is degraded, not fatal.
  if (vendor == 0xFF || chip == 0xFF) {
    dualPrintln("[touch] FT6236 not responding on I2C");
  } else {
    dualPrintf("[touch] FT6236 ready (vendor=0x%02X chip=0x%02X)\n",
              (unsigned)vendor, (unsigned)chip);
  }
}

static bool boardTouchPressed() {
  uint8_t points = ft6236ReadReg(FT6236_REG_TD_STATUS);
  // TD_STATUS: 0 = no touch, 1-2 = touch points, 0xFF = read failure.
  // A read failure is treated as "not pressed" so a missing/broken
  // controller can never fake a stuck touch.
  return points >= 1 && points <= 2;
}

static void boardTouchDebug() {
  uint8_t points = ft6236ReadReg(FT6236_REG_TD_STATUS);
  uint8_t xh = ft6236ReadReg(FT6236_REG_XH);
  uint8_t xl = ft6236ReadReg(FT6236_REG_XL);
  uint8_t yh = ft6236ReadReg(FT6236_REG_YH);
  uint8_t yl = ft6236ReadReg(FT6236_REG_YL);
  unsigned x = ((unsigned)(xh & 0x0F) << 8) | xl;
  unsigned y = ((unsigned)(yh & 0x0F) << 8) | yl;
  dualPrintf(
      "{\"event\":\"touch_status\",\"points\":%u,\"x\":%u,\"y\":%u}\n",
      (unsigned)points, x, y);
}

#endif // touch board selection

static void cydInitDisplay() {
#if CYD_BUILD
  // CYD drives its own backlight pin; CrowPanel's BL is handled by TFT_eSPI
  // via the TFT_BL build flag, and has no separate boot button.
  pinMode(CYD_TFT_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(CYD_TFT_BACKLIGHT_PIN, HIGH);
  pinMode(CYD_BOOT_BUTTON_PIN, INPUT_PULLUP);
#endif
  boardTouchInit();

  tft.init();
  tft.setRotation(cydTftRotation);
  cydScreenW = tft.width();
  cydScreenH = tft.height();
  tft.fillScreen(CYD_COLOR_BG);
  tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_BG);
  tft.setTextDatum(TL_DATUM);
  cydDisplayReady = true;
}

static void cydDrawHeader(const char* title) {
  tft.fillRect(0, 0, cydScreenW, 36, CYD_COLOR_NAVY);
  tft.drawFastHLine(0, 35, cydScreenW, CYD_COLOR_CYAN);
  tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_NAVY);
  tft.setTextSize(2);
  tft.drawString(FY_UI_BRAND, 8, 4);
  tft.setTextSize(1);
  tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_NAVY);
  tft.drawString(title, 10, 24);
#if CYD_BUILD
  // "BTN" badge hints the CYD boot button rotates the screen. CrowPanel has
  // no spare button, so the badge is omitted there (touch cycles screens).
  tft.fillRoundRect(cydScreenW - 42, 8, 34, 18, 9, CYD_COLOR_SURFACE_2);
  tft.drawRoundRect(cydScreenW - 42, 8, 34, 18, 9, CYD_COLOR_CYAN);
  tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_SURFACE_2);
  tft.drawString("BTN", cydScreenW - 35, 13);
#endif
}

static void cydDrawUi(bool force = false) {
  if (!cydDisplayReady) return;
  unsigned long now = millis();

  // ── Full-screen Flock Found flash overlay ──────────────────
  if (cydFlashActive) {
    if (now - cydFlashStartMs < CYD_FLASH_DURATION_MS) {
      tft.fillScreen(CYD_COLOR_DANGER);
      tft.setTextSize(3);
      tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_DANGER);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("FLOCK FOUND", cydScreenW / 2, cydScreenH / 2 - 10);
      // Show detection source (WiFi / BLE)
      tft.setTextSize(2);
      if (cydLastMethod[0]) {
        const char* srcLabel = "WIFI";
        if (strstr(cydLastMethod, "ble")) srcLabel = "BLE";
        tft.drawString(srcLabel, cydScreenW / 2, cydScreenH / 2 + 20);
      }
      tft.setTextSize(1);
      tft.setTextColor(CYD_COLOR_MUTED, CYD_COLOR_DANGER);
      tft.drawString("touch to dismiss", cydScreenW / 2, cydScreenH / 2 + 48);
      tft.setTextDatum(TL_DATUM);  // restore default
      return;  // skip normal UI while flash is showing
    } else {
      cydFlashActive = false;
      force = true;  // force full redraw of normal UI when flash ends
    }
  }

  if (!force && now - cydLastUiDraw < CYD_UI_REFRESH_MS) return;
  cydLastUiDraw = now;
  char timeBuf[8];
  bool fullRedraw = force || cydLastDrawnScreen != cydScreen;

  if (fullRedraw) {
    tft.fillRect(0, 0, cydScreenW, cydScreenH, CYD_COLOR_BG);
    cydLastDrawnScreen = cydScreen;
  }
  // No incremental clear: each panel/metric box fills its own background,
  // so clearing the whole content area every second causes visible flicker.
  bool recentHit = cydLastDetectionMs && now - cydLastDetectionMs < 15000;

  // BLE Flock scan status indicator for SCAN screen.
  // BLE Flock scanning runs simultaneously with phone BLE UART — the ESP32
  // Bluedroid stack supports both peripheral (server) and central (scanner)
  // roles at the same time.
  char bleVal[12];
  uint16_t bleCol;
  if (bleFlockDetCount > 0) {
    snprintf(bleVal, sizeof(bleVal), "%d", bleFlockDetCount);
    bleCol = CYD_COLOR_BLUE;
  } else if (bleFlockScanning) {
    strlcpy(bleVal, cydBleClientConnected ? "BT+SCAN" : "SCAN", sizeof(bleVal));
    bleCol = CYD_COLOR_CYAN;
  } else {
    strlcpy(bleVal, cydBleClientConnected ? "BT+IDLE" : "IDLE", sizeof(bleVal));
    bleCol = CYD_COLOR_MUTED;
  }

  switch (cydScreen) {
    case SCREEN_SCAN:
      if (fullRedraw) {
        cydDrawHeader("SCAN");
      }
      if (cydScreenW >= 320) {
        // Landscape layout
        cydDrawPanel(8, 44, 202, 54);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("RF STATUS", 18, 50);
        tft.setTextSize(4);
        tft.setTextColor(recentHit ? CYD_COLOR_BLUE : CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString(recentHit ? "HIT" : "SCAN", 18, 62);
        cydDrawFlockFreeMark(226, 48, 78, 42);

        cydDrawMetricNumberBox(8, 110, 72, 44, "CH", currentChannel, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(88, 110, 104, 44, "HITS", fyDetCount, recentHit ? CYD_COLOR_BLUE : CYD_COLOR_TEXT);
        cydDrawMetricBox(200, 110, 112, 44, "GPS", cydGpsFresh() ? "OK" : "WAIT", cydBoolColor(cydGpsFresh()));
        cydDrawMetricBox(8, 162, 64, 36, "SD", cydSdReady ? "OK" : "MISS", cydSdReady ? CYD_COLOR_GREEN : CYD_COLOR_DANGER);
        cydDrawMetricBox(80, 162, 116, 36, "BLE", bleVal, bleCol);
        cydDrawMetricBox(204, 162, 108, 36, "MODE", channelModeName(), CYD_COLOR_TEXT);
      } else {
        // Portrait layout (240 wide)
        cydDrawPanel(8, 44, cydScreenW - 16, 54);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("RF STATUS", 18, 50);
        tft.setTextSize(4);
        tft.setTextColor(recentHit ? CYD_COLOR_BLUE : CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString(recentHit ? "HIT" : "SCAN", 18, 62);

        int pw = (cydScreenW - 24) / 2;  // ~108
        cydDrawMetricNumberBox(8, 110, pw, 44, "CH", currentChannel, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(8 + pw + 8, 110, pw, 44, "HITS", fyDetCount, recentHit ? CYD_COLOR_BLUE : CYD_COLOR_TEXT);
        cydDrawMetricBox(8, 162, cydScreenW - 16, 36, "GPS", cydGpsFresh() ? "OK" : "WAIT", cydBoolColor(cydGpsFresh()));
        cydDrawMetricBox(8, 206, 68, 36, "SD", cydSdReady ? "OK" : "MISS", cydSdReady ? CYD_COLOR_GREEN : CYD_COLOR_DANGER);
        cydDrawMetricBox(84, 206, 68, 36, "BLE", bleVal, bleCol);
        cydDrawMetricBox(160, 206, 72, 36, "MODE", channelModeName(), CYD_COLOR_TEXT);
      }
      cydDrawDashboardFooter(now);
      break;

    case SCREEN_GPS:
      if (fullRedraw) cydDrawHeader("Phone GPS");
      if (cydScreenW >= 320) {
        // Landscape layout
        cydDrawMetricBox(8, 44, 304, 42, "PHONE GPS",
                         cydGpsFresh() ? "GPS OK" : "NO GPS",
                         cydBoolColor(cydGpsFresh()));

        cydDrawPanel(8, 94, 146, 44);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("LAT", 16, 100);
        tft.setTextSize(2);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_SURFACE);
        tft.drawFloat(cydGps.lat, 5, 16, 114);

        cydDrawPanel(166, 94, 146, 44);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("LON", 174, 100);
        tft.setTextSize(2);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_SURFACE);
        tft.drawFloat(cydGps.lng, 5, 174, 114);

        cydDrawPanel(8, 150, 146, 44);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("ACCURACY", 16, 156);
        tft.setTextSize(2);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_SURFACE);
        tft.drawFloat(cydGps.accuracyM, 1, 16, 170);
        tft.drawString("m", 82, 170);

        cydDrawMetricNumberBox(166, 150, 146, 44, "AGE SEC",
                               cydGps.hasFix ? (unsigned long)((now - cydGps.lastFixMs) / 1000) : 0,
                               cydGpsFresh() ? CYD_COLOR_GREEN : CYD_COLOR_AMBER);
      } else {
        // Portrait layout (240 wide)
        int pw = cydScreenW - 16;  // 224
        cydDrawMetricBox(8, 44, pw, 42, "PHONE GPS",
                         cydGpsFresh() ? "GPS OK" : "NO GPS",
                         cydBoolColor(cydGpsFresh()));

        int halfW = (pw - 8) / 2;  // ~108
        cydDrawPanel(8, 94, halfW, 44);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("LAT", 16, 100);
        tft.setTextSize(2);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_SURFACE);
        tft.drawFloat(cydGps.lat, 4, 16, 114);

        cydDrawPanel(8 + halfW + 8, 94, halfW, 44);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("LON", 16 + halfW + 8, 100);
        tft.setTextSize(2);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_SURFACE);
        tft.drawFloat(cydGps.lng, 4, 16 + halfW + 8, 114);

        cydDrawPanel(8, 150, halfW, 44);
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_CYAN, CYD_COLOR_SURFACE);
        tft.drawString("ACCURACY", 16, 156);
        tft.setTextSize(2);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_SURFACE);
        tft.drawFloat(cydGps.accuracyM, 1, 16, 170);
        tft.drawString("m", 82, 170);

        cydDrawMetricNumberBox(8 + halfW + 8, 150, halfW, 44, "AGE SEC",
                               cydGps.hasFix ? (unsigned long)((now - cydGps.lastFixMs) / 1000) : 0,
                               cydGpsFresh() ? CYD_COLOR_GREEN : CYD_COLOR_AMBER);
      }
      break;

    case SCREEN_LOG:
      if (fullRedraw) cydDrawHeader("CSV Log");
      if (cydScreenW >= 320) {
        cydDrawMetricBox(8, 44, 304, 42, "CARD", cydSdReady ? "SD MOUNTED" : "SD UNAVAILABLE",
                         cydSdReady ? CYD_COLOR_GREEN : CYD_COLOR_DANGER);
        cydDrawMetricBox(8, 98, 304, 42, "FILE", CYD_LOG_FILE, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(8, 152, 146, 44, "ROWS", cydCsvRows, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(166, 152, 146, 44, "FAILURES", cydSdFailures,
                               cydSdFailures ? CYD_COLOR_DANGER : CYD_COLOR_GREEN);
      } else {
        int pw = cydScreenW - 16;
        int halfW = (pw - 8) / 2;
        cydDrawMetricBox(8, 44, pw, 42, "CARD", cydSdReady ? "SD MOUNTED" : "SD UNAVAILABLE",
                         cydSdReady ? CYD_COLOR_GREEN : CYD_COLOR_DANGER);
        cydDrawMetricBox(8, 98, pw, 42, "FILE", CYD_LOG_FILE, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(8, 152, halfW, 44, "ROWS", cydCsvRows, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(8 + halfW + 8, 152, halfW, 44, "FAILURES", cydSdFailures,
                               cydSdFailures ? CYD_COLOR_DANGER : CYD_COLOR_GREEN);
      }
      break;

    case SCREEN_LAST:
      if (fullRedraw) cydDrawHeader("Last Detection");
      if (cydScreenW >= 320) {
        cydDrawMetricBox(8, 44, 304, 42, "DEVICE", cydLastMac[0] ? cydLastMac : "NONE YET",
                         cydLastMac[0] ? CYD_COLOR_TEXT : CYD_COLOR_MUTED);
        cydDrawMetricBox(8, 98, 304, 42, "METHOD", cydLastMethod[0] ? cydLastMethod : "-",
                         CYD_COLOR_TEXT);
        cydDrawMetricSignedBox(8, 152, 72, 40, "RSSI", (long)cydLastRssi, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(88, 152, 72, 40, "CH", cydLastChannel, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(168, 152, 72, 40, "HITS", cydLastCount, CYD_COLOR_BLUE);
        cydDrawMetricBox(248, 152, 64, 40, "BT", cydBleClientConnected ? "OK" : "WAIT",
                         cydBoolColor(cydBleClientConnected));
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_MUTED, CYD_COLOR_BG);
        cydFormatClock(timeBuf, sizeof(timeBuf), now);
        tft.drawString("TIME", 8, 205);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_BG);
        tft.drawString(timeBuf, 50, 205);
        tft.setTextColor(CYD_COLOR_MUTED, CYD_COLOR_BG);
        tft.drawString("LOC", 8, 222);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_BG);
      if (cydLastLocationValid) {
        tft.drawFloat(cydLastLat, 5, 42, 222);
        tft.drawString(",", 104, 222);
        tft.drawFloat(cydLastLng, 5, 116, 222);
      } else {
        tft.drawString("none captured", 42, 222);
      }
      } else {
        // Portrait layout (240 wide)
        int pw = cydScreenW - 16;  // 224
        cydDrawMetricBox(8, 44, pw, 42, "DEVICE", cydLastMac[0] ? cydLastMac : "NONE YET",
                         cydLastMac[0] ? CYD_COLOR_TEXT : CYD_COLOR_MUTED);
        cydDrawMetricBox(8, 98, pw, 42, "METHOD", cydLastMethod[0] ? cydLastMethod : "-",
                         CYD_COLOR_TEXT);
        int halfW = (pw - 8) / 2;
        cydDrawMetricSignedBox(8, 152, halfW, 40, "RSSI", (long)cydLastRssi, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(8 + halfW + 8, 152, halfW, 40, "CH", cydLastChannel, CYD_COLOR_TEXT);
        cydDrawMetricNumberBox(8, 198, halfW, 40, "HITS", cydLastCount, CYD_COLOR_BLUE);
        cydDrawMetricBox(8 + halfW + 8, 198, halfW, 40, "BT", cydBleClientConnected ? "OK" : "WAIT",
                         cydBoolColor(cydBleClientConnected));
        tft.setTextSize(1);
        tft.setTextColor(CYD_COLOR_MUTED, CYD_COLOR_BG);
        cydFormatClock(timeBuf, sizeof(timeBuf), now);
        tft.drawString("TIME", 8, 252);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_BG);
        tft.drawString(timeBuf, 50, 252);
        tft.setTextColor(CYD_COLOR_MUTED, CYD_COLOR_BG);
        tft.drawString("LOC", 8, 270);
        tft.setTextColor(CYD_COLOR_TEXT, CYD_COLOR_BG);
        if (cydLastLocationValid) {
          tft.drawFloat(cydLastLat, 4, 42, 270);
          tft.drawString(",", 90, 270);
          tft.drawFloat(cydLastLng, 4, 100, 270);
        } else {
          tft.drawString("none captured", 42, 270);
        }
      }
      break;

    default:
      cydScreen = SCREEN_SCAN;
      break;
  }
}

static void cydSetDisplayRotation(uint8_t rotation, bool redraw) {
  cydTftRotation = rotation % 4;
  tft.setRotation(cydTftRotation);
  cydScreenW = tft.width();
  cydScreenH = tft.height();
  if (redraw) {
    tft.fillScreen(CYD_COLOR_BG);
    cydDrawUi(true);
  }
}

// --- Screen-rotation persistence (FYROTATE command) -----------------------
// One ASCII digit ('0'..'3') in /rotation, stored with the session-save
// pattern (tmp write -> read-back validation -> atomic promote). A missing
// or corrupt file simply falls back to the compiled CYD_TFT_ROTATION
// default — worst case is one boot at the default orientation, never a
// boot failure — so the single-byte preference needs no tmp-file recovery.
static bool fySaveRotation(uint8_t rotation) {
  if (!fySpiffsReady) return false;

  char digit = (char)('0' + (rotation % 4));
  File f = SPIFFS.open(FY_ROTATION_TMP, "w");
  if (!f) return false;
  f.write((uint8_t)digit);
  f.close();

  File v = SPIFFS.open(FY_ROTATION_TMP, "r");
  bool ok = v && v.available() == 1 && (char)v.read() == digit;
  if (v) v.close();
  if (!ok) return false;

  SPIFFS.remove(FY_ROTATION_FILE);
  return fyAtomicPromote(FY_ROTATION_TMP, FY_ROTATION_FILE);
}

// Returns the persisted rotation, or the compiled default when the file is
// absent/corrupt or SPIFFS is down. Never fails.
static uint8_t fyLoadRotation() {
  if (!fySpiffsReady) return CYD_TFT_ROTATION;
  File f = SPIFFS.open(FY_ROTATION_FILE, "r");
  if (!f) return CYD_TFT_ROTATION;
  int c = f.read();
  f.close();
  if (c < '0' || c > '3') return CYD_TFT_ROTATION;
  dualPrintf("[flockyou] rotation %u restored from SPIFFS\n",
             (unsigned)(c - '0'));
  return (uint8_t)(c - '0');
}

static void cydButtonTick() {
#if CYD_BUILD
  // CYD's boot button rotates the display. CrowPanel has no spare button
  // (GPIO0 is the BOOT strap), so rotation there is via touch/screens or
  // the FYSCREEN command.
  bool state = digitalRead(CYD_BOOT_BUTTON_PIN);
  unsigned long now = millis();
  if (cydLastButtonState == HIGH && state == LOW && now - cydLastButtonAt > 250) {
    cydLastButtonAt = now;
    cydSetDisplayRotation((cydTftRotation + 1) % 4, true);
    dualPrintf("[cyd] button -> rotation %u\n", (unsigned)cydTftRotation);
  }
  cydLastButtonState = state;
#else
  (void)0;  // no button on this board
#endif
}

static void cydTouchTick() {
  bool down = boardTouchPressed();
  unsigned long now = millis();
  if (down && !cydLastTouchDown && now - cydLastTouchMs > CYD_ROTATION_DEBOUNCE_MS) {
    cydLastTouchMs = now;

    // If flash overlay is active, any touch dismisses it immediately
    if (cydFlashActive) {
      cydFlashActive = false;
      cydDrawUi(true);
      dualPrintf("[cyd] touch -> flash dismissed\n");
      cydLastTouchDown = down;
      return;
    }

    cydScreen = (CydScreen)(((uint8_t)cydScreen + 1) % SCREEN_COUNT);
    cydDrawUi(true);
#if FY_TOUCH_XPT2046_BITBANG
    // Preserve the CYD debug line verbatim (raw last sample, may be stale
    // if the finger lifted mid-read — it was the same before the port).
    dualPrintf("[cyd] touch -> screen %u x=%u y=%u z=%u\n",
               (unsigned)cydScreen, (unsigned)cydTouchLastX,
               (unsigned)cydTouchLastY, (unsigned)cydTouchLastZ);
#else
    dualPrintf("[cyd] touch -> screen %u\n", (unsigned)cydScreen);
#endif
  }
  cydLastTouchDown = down;
}

static void cydInitSd() {
  cydSdReady = SD.begin(CYD_SD_CS_PIN);
  if (!cydSdReady) {
    dualPrintln("[cyd] SD init failed; CSV logging disabled");
    return;
  }

  if (!SD.exists(CYD_LOG_FILE)) {
    File f = SD.open(CYD_LOG_FILE, FILE_WRITE);
    if (f) {
      f.println("millis,mac,oui,method,rssi,channel,frequency_mhz,lat,lon,accuracy_m,gps_age_ms,speed_kmph,course_deg,count");
      f.close();
    }
  }
  dualPrintln("[cyd] SD ready; CSV logging enabled");
}

static void cydLogDetectionCsv(const char* mac, const char* oui, const char* method,
                               int8_t rssi, uint8_t ch, uint16_t count) {
  if (!cydSdReady) return;
  File f = SD.open(CYD_LOG_FILE, FILE_APPEND);
  if (!f) {
    cydSdFailures++;
    return;
  }

  unsigned long now = millis();
  long gpsAge = cydGps.hasFix ? (long)(now - cydGps.lastFixMs) : -1;
  f.printf("%lu,%s,%s,%s,%d,%u,%u,",
           now, mac, oui, method, (int)rssi, (unsigned)ch,
           (unsigned)channelFreqMhz(ch));
  if (cydGps.hasFix) {
    f.printf("%.6f,%.6f,%.1f,%ld,%.1f,%.1f,%u\n",
             cydGps.lat, cydGps.lng, cydGps.accuracyM, gpsAge,
             cydGps.speedKmph, cydGps.courseDeg, (unsigned)count);
  } else {
    f.printf(",,,,,,%u\n", (unsigned)count);
  }
  f.close();
  cydCsvRows++;
}

static void cydRecordDetection(const char* mac, const char* oui, const char* method,
                               int8_t rssi, uint8_t ch, uint16_t count) {
  strlcpy(cydLastMac, mac, sizeof(cydLastMac));
  strlcpy(cydLastMethod, method, sizeof(cydLastMethod));
  cydLastRssi = rssi;
  cydLastChannel = ch;
  cydLastCount = count;
  cydLastDetectionMs = millis();
  cydLastLocationValid = cydGps.hasFix;
  if (cydLastLocationValid) {
    cydLastLat = cydGps.lat;
    cydLastLng = cydGps.lng;
  }
  cydLogDetectionCsv(mac, oui, method, rssi, ch, count);
  cydFlashActive = true;
  cydFlashStartMs = millis();
  cydDrawUi(true);
}

static void cydEmitSimulatedDetection() {
  static uint8_t simCounter = 0;
  char mac[18];
  snprintf(mac, sizeof(mac), "70:c9:4e:fa:ce:%02x", simCounter++);

  const char* method = "wildcard_probe";
  const char* ssid = "Flock-Test";
  const int8_t rssi = -42;
  const uint8_t channel = currentChannel ? currentChannel : 6;
  char oui[9] = "70:c9:4e";

  bool chirpWorthy = false;
  int idx = fyAddDetection(mac, method, rssi, channel, ssid, &chirpWorthy);
  uint16_t count = (idx >= 0) ? fyDet[idx].count : 0;
  fyLastTargetSeen = millis();

  dualPrintf("[cyd] simulated detection mac=%s rssi=%d ch=%u\n",
             mac, (int)rssi, (unsigned)channel);
  cydRecordDetection(mac, oui, method, rssi, channel, count);
  emitDetectionJSON(mac, method, rssi, channel, ssid);

  if (chirpWorthy) {
    newDetectChirp();
    fyLastHeartbeatAt = millis();
  }
  ledFlash(LED_FLASH_MS);
}

static void cydParseGpsCsv(char* line) {
  char* save = nullptr;
  strtok_r(line, ",", &save); // command
  char* latTok = strtok_r(nullptr, ",", &save);
  char* lonTok = strtok_r(nullptr, ",", &save);
  if (!latTok || !lonTok) return;

  double lat = atof(latTok);
  double lon = atof(lonTok);
  double acc = 0;
  double speed = 0;
  double course = 0;
  uint32_t sats = 0;
  double hdop = 0;

  char* tok = strtok_r(nullptr, ",", &save); if (tok) acc = atof(tok);
  tok = strtok_r(nullptr, ",", &save); if (tok) speed = atof(tok);
  tok = strtok_r(nullptr, ",", &save); if (tok) course = atof(tok);
  tok = strtok_r(nullptr, ",", &save); if (tok) sats = (uint32_t)strtoul(tok, nullptr, 10);
  tok = strtok_r(nullptr, ",", &save); if (tok) hdop = atof(tok);
  tok = strtok_r(nullptr, ",", &save);
  uint32_t unixTime = tok ? (uint32_t)strtoul(tok, nullptr, 10) : 0;
  tok = strtok_r(nullptr, ",", &save);
  int16_t utcOffsetMin = tok ? (int16_t)atoi(tok) : 0;

  cydSetGps(lat, lon, acc, speed, course, sats, hdop,
            unixTime, utcOffsetMin, "phone");
}

static void cydParseNmeaLine(const char* line) {
  for (const char* p = line; *p; p++) {
    phoneGps.encode(*p);
  }
  phoneGps.encode('\n');
  if (phoneGps.location.isUpdated() && phoneGps.location.isValid()) {
    cydSetGps(
        phoneGps.location.lat(),
        phoneGps.location.lng(),
        phoneGps.hdop.isValid() ? phoneGps.hdop.hdop() * 5.0 : 0,
        phoneGps.speed.isValid() ? phoneGps.speed.kmph() : 0,
        phoneGps.course.isValid() ? phoneGps.course.deg() : 0,
        phoneGps.satellites.isValid() ? phoneGps.satellites.value() : 0,
        phoneGps.hdop.isValid() ? phoneGps.hdop.hdop() : 0,
        0,
        0,
        "nmea");
  }
}

static void cydHandleCommand(char* line) {
  if (!line || !line[0]) return;
  if (strcmp(line, "FYHELLO") == 0 || strcmp(line, "FYSTATUS") == 0) {
    cydEmitPairStatus();
    return;
  }
  if (strcmp(line, "FYSCREEN,next") == 0) {
    cydScreen = (CydScreen)(((uint8_t)cydScreen + 1) % SCREEN_COUNT);
    cydDrawUi(true);
    return;
  }
  if (strncmp(line, "FYROTATE", 8) == 0) {
    const char* arg = line + 8;
    uint8_t target;
    if (strcmp(arg, ",next") == 0) {
      target = (uint8_t)((cydTftRotation + 1) % 4);
    } else if (arg[0] == ',' && arg[1] >= '0' && arg[1] <= '3' && arg[2] == '\0') {
      target = (uint8_t)(arg[1] - '0');
    } else {
      dualPrintln("{\"event\":\"rotate_error\",\"error\":\"usage\","
                 "\"usage\":\"FYROTATE,next | FYROTATE,<0-3>\"}");
      return;
    }
    cydSetDisplayRotation(target, true);  // redraw=true repaints the UI
    bool saved = fySaveRotation(cydTftRotation);
    dualPrintf("{\"event\":\"rotate\",\"rotation\":%u,\"saved\":%s}\n",
               (unsigned)cydTftRotation, saved ? "true" : "false");
    return;
  }
  if (strcmp(line, "FYTOUCH") == 0) {
    boardTouchDebug();
    return;
  }
  if (strcmp(line, "FYSIM") == 0) {
    cydEmitSimulatedDetection();
    return;
  }
  if (strncmp(line, "FYGPS,", 6) == 0) {
    cydParseGpsCsv(line);
    cydDrawUi(false);
    return;
  }
  if (line[0] == '$') {
    cydParseNmeaLine(line);
    cydDrawUi(false);
  }
}

static bool cydBleEnqueueLine(const char* line) {
  if (!line || !line[0]) return true;

  portENTER_CRITICAL(&cydBleRxMux);
  uint8_t next = (uint8_t)((cydBleRxHead + 1) % CYD_BLE_RX_QUEUE_SIZE);
  if (next == cydBleRxTail) {
    portEXIT_CRITICAL(&cydBleRxMux);
    return false;
  }
  strlcpy(cydBleRxQueue[cydBleRxHead], line, CYD_BLE_RX_LINE_SIZE);
  cydBleRxHead = next;
  portEXIT_CRITICAL(&cydBleRxMux);
  return true;
}

static bool cydBleDequeueLine(char* out, size_t cap) {
  if (!out || cap == 0) return false;

  portENTER_CRITICAL(&cydBleRxMux);
  if (cydBleRxHead == cydBleRxTail) {
    portEXIT_CRITICAL(&cydBleRxMux);
    return false;
  }
  strlcpy(out, cydBleRxQueue[cydBleRxTail], cap);
  cydBleRxTail = (uint8_t)((cydBleRxTail + 1) % CYD_BLE_RX_QUEUE_SIZE);
  portEXIT_CRITICAL(&cydBleRxMux);
  return true;
}

static void cydBleDrainCommands() {
  char line[CYD_BLE_RX_LINE_SIZE];
  while (cydBleDequeueLine(line, sizeof(line))) {
    cydHandleCommand(line);
  }
}

class CydBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    cydBleClientConnected = true;
    cydBleConnectionChanged = true;
  }

  void onDisconnect(BLEServer* server) override {
    cydBleClientConnected = false;
    cydBleConnectionChanged = true;
    server->getAdvertising()->start();
  }
};

class CydBleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    std::string value = characteristic->getValue();
    static char line[CYD_BLE_RX_LINE_SIZE];
    static size_t len = 0;

    for (size_t i = 0; i < value.length(); i++) {
      char c = value[i];
      if (c == '\r') continue;
      if (c == '\n') {
        line[len] = '\0';
        cydBleEnqueueLine(line);
        len = 0;
      } else if (len + 1 < sizeof(line)) {
        line[len++] = c;
      } else {
        len = 0;
      }
    }
  }
};

static void cydBleWriteBytes(const uint8_t* data, size_t len) {
  if (!cydBleReady || !cydBleClientConnected || !cydBleTx || !data || len == 0) {
    return;
  }

  for (size_t offset = 0; offset < len; offset += CYD_BLE_CHUNK_BYTES) {
    size_t chunk = len - offset;
    if (chunk > CYD_BLE_CHUNK_BYTES) chunk = CYD_BLE_CHUNK_BYTES;
    cydBleTx->setValue((uint8_t*)data + offset, chunk);
    cydBleTx->notify();
    delay(3);
  }
}

static void cydBleInit() {
  BLEDevice::init(CYD_PAIR_NAME);
  BLEDevice::setMTU(185);

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new CydBleServerCallbacks());

  BLEService* service = server->createService(CYD_BLE_SERVICE_UUID);
  cydBleTx = service->createCharacteristic(
      CYD_BLE_TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  cydBleTx->addDescriptor(new BLE2902());

  BLECharacteristic* rx = service->createCharacteristic(
      CYD_BLE_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new CydBleRxCallbacks());

  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(CYD_BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
  cydBleReady = true;
}

static void cydSerialTick() {
  static char line[180];
  static size_t len = 0;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      cydHandleCommand(line);
      len = 0;
    } else if (len + 1 < sizeof(line)) {
      line[len++] = c;
    } else {
      len = 0;
    }
  }

  if (millis() - cydLastPairAnnounce > 5000) {
    cydEmitPairStatus();
    cydLastPairAnnounce = millis();
  }
}


// ============================================================
// BLE FLOCK BATTERY DETECTION
// ============================================================

class BleFlockAdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    int rssi = advertisedDevice.getRSSI();
    if (rssi < CYD_BLE_FLOCK_RSSI_MIN) return;

    std::string name = advertisedDevice.getName();
    std::string addr = advertisedDevice.getAddress().toString();
    bool isFlock = false;
    bool isXuntong = false;

    // Check device name patterns
    if (!name.empty()) {
      // "Penguin-NNNNNNNNNN" (10 digits)
      if (name.length() == 18 && strncmp(name.c_str(), "Penguin-", 8) == 0) {
        bool allDigits = true;
        for (size_t i = 8; i < 18; i++) {
          if (name[i] < '0' || name[i] > '9') { allDigits = false; break; }
        }
        if (allDigits) isFlock = true;
      }
      // "FS Ext Battery"
      else if (name == "FS Ext Battery") {
        isFlock = true;
      }
      // Pure 10-digit name (post March 2025 Penguin firmware update)
      // Cross-check against known Flock OUI prefixes to reduce false positives
      // from other IoT devices with numeric names.
      else if (name.length() == 10) {
        bool allDigits = true;
        for (size_t i = 0; i < 10; i++) {
          if (name[i] < '0' || name[i] > '9') { allDigits = false; break; }
        }
        if (allDigits) {
          // OUI cross-check: use Bluedroid's getNative() which returns esp_bd_addr_t*
          esp_bd_addr_t* macNative = advertisedDevice.getAddress().getNative();
          bool ouiMatch = false;
          for (size_t i = 0; i < OUI_COUNT; i++) {
            if ((*macNative)[0] == oui_bytes[i][0] &&
                (*macNative)[1] == oui_bytes[i][1] &&
                (*macNative)[2] == oui_bytes[i][2]) { ouiMatch = true; break; }
          }
          // Also check LAA prefixes
          for (size_t i = 0; i < OUI_LAA_COUNT; i++) {
            if ((*macNative)[0] == oui_laa_bytes[i][0] &&
                (*macNative)[1] == oui_laa_bytes[i][1] &&
                (*macNative)[2] == oui_laa_bytes[i][2]) { ouiMatch = true; break; }
          }
          if (ouiMatch) isFlock = true;
        }
      }
    }

    // Check for XUNTONG manufacturer specific data (Company ID 0x09C8)
    if (advertisedDevice.haveManufacturerData()) {
      std::string mdata = advertisedDevice.getManufacturerData();
      if (mdata.length() >= 2) {
        uint16_t companyId = (uint16_t)((uint8_t)mdata[0] | ((uint8_t)mdata[1] << 8));
        if (companyId == XUNTONG_COMPANY_ID) {
          isFlock = true;
          isXuntong = true;
        }
      }
    }

    if (!isFlock) return;

    // Match found — store it
    char macStr[18];
    strncpy(macStr, addr.c_str(), sizeof(macStr) - 1);
    macStr[sizeof(macStr) - 1] = '\0';
    for (char* p = macStr; *p; p++) *p = tolower((unsigned char)*p);

    char nameStr[32];
    strncpy(nameStr, name.c_str(), sizeof(nameStr) - 1);
    nameStr[sizeof(nameStr) - 1] = '\0';

    bool isNew = true;
    bool chirpWorthy = true;
    uint16_t hitCount = 1;
    for (int i = 0; i < bleFlockDetCount; i++) {
      if (strcasecmp(bleFlockDetections[i].mac, macStr) == 0) {
        bool rediscover = (millis() - bleFlockDetections[i].lastSeen) > REDISCOVER_MS;
        bleFlockDetections[i].lastSeen = millis();
        bleFlockDetections[i].rssi = (int8_t)rssi;
        if (bleFlockDetections[i].count < 0xFFFF) bleFlockDetections[i].count++;
        hitCount = bleFlockDetections[i].count;
        isNew = false;
        chirpWorthy = rediscover;
        break;
      }
    }
    if (isNew && bleFlockDetCount < CYD_BLE_FLOCK_MAX_DETECTIONS) {
      BleFlockDetection& d = bleFlockDetections[bleFlockDetCount];
      strncpy(d.mac, macStr, sizeof(d.mac) - 1);
      d.mac[sizeof(d.mac) - 1] = '\0';
      strncpy(d.name, nameStr, sizeof(d.name) - 1);
      d.name[sizeof(d.name) - 1] = '\0';
      d.rssi = (int8_t)rssi;
      d.firstSeen = millis();
      d.lastSeen = millis();
      d.count = 1;
      d.isXuntong = isXuntong;
      hitCount = d.count;
      bleFlockDetCount++;
    }

    // Emit JSON detection
    char oui[9] = "";
    if (strlen(macStr) >= 8) { strncpy(oui, macStr, 8); oui[8] = '\0'; }

#if FY_UI_BUILD
    // Record detection for CYD display (triggers red flash screen)
    cydRecordDetection(macStr, oui, "ble_flock_battery", (int8_t)rssi, 0, hitCount);
#endif

    char bleGpsSuffix[180] = "";
    if (cydGps.hasFix) {
      snprintf(bleGpsSuffix, sizeof(bleGpsSuffix),
               ",\"gps\":{\"latitude\":%.6f,\"longitude\":%.6f,\"accuracy\":%.1f,\"age_ms\":%lu,\"source\":\"%s\"}",
               cydGps.lat, cydGps.lng, cydGps.accuracyM,
               (unsigned long)(millis() - cydGps.lastFixMs), cydGps.source);
    }

    char nameEsc[sizeof(nameStr) * 6 + 1];
    jsonEscape(nameEsc, sizeof(nameEsc), nameStr);

    dualPrintf(
        "{\"event\":\"detection\","
        "\"detection_method\":\"ble_flock_battery\","
        "\"protocol\":\"ble\","
        "\"mac_address\":\"%s\","
        "\"oui\":\"%s\","
        "\"device_name\":\"%s\","
        "\"rssi\":%d,"
        "\"channel\":0,"
        "\"frequency\":2400,"
        "\"ssid\":\"\""
        "%s"
        "}\n",
        macStr, oui, nameEsc, rssi, bleGpsSuffix);

    // Log to SD card
    if (cydSdReady) {
      File f = SD.open(CYD_LOG_FILE, FILE_APPEND);
      if (f) {
        unsigned long now = millis();
        long gpsAge = cydGps.hasFix ? (long)(now - cydGps.lastFixMs) : -1;
        f.printf("%lu,%s,%s,ble_battery,%d,0,2400,", now, macStr, oui, rssi);
        if (cydGps.hasFix) {
          f.printf("%.6f,%.6f,%.1f,%ld,%.1f,%.1f,%u\n",
                   cydGps.lat, cydGps.lng, cydGps.accuracyM, gpsAge,
                   cydGps.speedKmph, cydGps.courseDeg, (unsigned)hitCount);
        } else {
          f.printf(",,,,,,%u\n", (unsigned)hitCount);
        }
        f.close();
        cydCsvRows++;
      }
    }

    // Update display
    strlcpy(cydLastMac, macStr, sizeof(cydLastMac));
    strlcpy(cydLastMethod, "ble_battery", sizeof(cydLastMethod));
    cydLastRssi = rssi;
    cydLastChannel = 0;
    cydLastCount = hitCount;
    cydLastDetectionMs = millis();
    cydLastLocationValid = cydGpsFresh();
    if (cydLastLocationValid) {
      cydLastLat = cydGps.lat;
      cydLastLng = cydGps.lng;
    }

    fyLastTargetSeen = millis();

    // Trigger full-screen Flock Found flash
    cydFlashActive = true;
    cydFlashStartMs = millis();
    cydDrawUi(true);

    if (chirpWorthy) {
      newDetectChirp();
      fyLastHeartbeatAt = millis();
    }
    ledFlash(LED_FLASH_MS);

    dualPrintf("[flockyou] BLE Flock detected: %s name=\"%s\" rssi=%d xuntong=%d\n",
               macStr, nameStr, rssi, isXuntong ? 1 : 0);
  }
};

static BLEScan* bleFlockScan = nullptr;

static void cydBleFlockStartScan() {
  if (bleFlockScanning) return;
  bleFlockScan = BLEDevice::getScan();
  static BleFlockAdvertisedCallbacks callbacks;
  bleFlockScan->setAdvertisedDeviceCallbacks(&callbacks, false);
  bleFlockScan->setActiveScan(true);
  bleFlockScan->setInterval(100);
  bleFlockScan->setWindow(99);
  // Note: Bluedroid BLEScan doesn't have setMaxResults() like NimBLE.
  // Unlimited results is the default behavior in Bluedroid.
  bleFlockScan->start(0, nullptr, false);
  bleFlockScanning = true;
  bleFlockScanStartMs = millis();
  dualPrintln("[flockyou] BLE Flock scan started");
}

static void cydBleFlockStopScan() {
  if (!bleFlockScanning) return;
  if (bleFlockScan) {
    bleFlockScan->stop();
  }
  bleFlockScanning = false;
  // Keep BLE advertising running for phone connection regardless of scan state
  BLEDevice::getAdvertising()->start();
  dualPrintf("[flockyou] BLE Flock scan done: %d detection(s)\n", bleFlockDetCount);
}

static void cydBleFlockTick() {
  unsigned long now = millis();
  // BLE Flock scanning runs regardless of phone BLE UART connection state.
  // The ESP32 Bluedroid stack supports simultaneous peripheral + central roles.
  if (bleFlockScanning) {
    if (now - bleFlockScanStartMs >= CYD_BLE_FLOCK_SCAN_DURATION_MS) {
      cydBleFlockStopScan();
      bleFlockLastScanMs = now;
    }
    return;
  }
  if (bleFlockLastScanMs == 0 || now - bleFlockLastScanMs >= CYD_BLE_FLOCK_SCAN_INTERVAL_MS) {
    cydBleFlockStartScan();
  }
}

static void cydInit() {
  cydInitDisplay();
  cydInitSd();
  cydBleInit();
  cydEmitPairStatus();
  cydDrawUi(true);
}
#endif

// ============================================================
// FLASK-COMPATIBLE JSON EMISSION
// ============================================================
//
// The Flask app (flock-you/api/flockyou.py) reads one JSON object per line
// from the USB CDC serial port. It filters by presence of `detection_method`
// and extracts these fields:  mac_address, rssi, channel, frequency, ssid,
// device_name, gps.latitude, gps.longitude, gps.accuracy.
//
// On CYD builds the paired phone can stream GPS into the device; when a fresh
// fix is present, detections include gps.* fields for the DeFlock fork.

static void emitDetectionJSON(const char* mac, const char* method,
                              int8_t rssi, uint8_t ch, const char* ssid,
                              const char* confidence) {
  char ssidEsc[sizeof(((FYDetection*)0)->ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), ssid ? ssid : "");
  char oui[9];
  uint8_t mbytes[6] = {0};
  sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
  ouiFromMac(mbytes, oui, sizeof(oui));

#if FY_UI_BUILD
  char gpsSuffix[180] = "";
  if (cydGps.hasFix) {
    snprintf(gpsSuffix, sizeof(gpsSuffix),
             ",\"gps\":{\"latitude\":%.6f,\"longitude\":%.6f,\"accuracy\":%.1f,\"age_ms\":%lu,\"source\":\"%s\"}",
             cydGps.lat, cydGps.lng, cydGps.accuracyM,
             (unsigned long)(millis() - cydGps.lastFixMs), cydGps.source);
  }
#endif

  dualPrintf(
      "{\"event\":\"detection\","
      "\"detection_method\":\"wifi_%s\","
      "\"protocol\":\"wifi_2_4ghz\","
      "\"mac_address\":\"%s\","
      "\"oui\":\"%s\","
      "\"device_name\":\"\","
      "\"rssi\":%d,"
      "\"channel\":%u,"
      "\"frequency\":%u,"
      "\"ssid\":\"%s\""
      "\"confidence\":\"%s\""
#if FY_UI_BUILD
      "%s"
#endif
      "}\n",
      method, mac, oui, rssi,
      (unsigned)ch, (unsigned)channelFreqMhz(ch), ssidEsc,
      confidence ? confidence : "unknown"
#if FY_UI_BUILD
      , gpsSuffix
#endif
      );
}

// ============================================================
// PROMISCUOUS CALLBACK  — keep it fast, no Serial, no malloc
// ============================================================

static bool IRAM_ATTR extractSsidFromMgmtBody(const uint8_t* body, int len,
                                     char* outSsid, size_t outLen) {
  if (!body || len <= 0 || !outSsid || outLen == 0) return false;
  while (len >= 2) {
    uint8_t id = body[0], elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) {
      size_t n = (elen < (outLen - 1)) ? elen : (outLen - 1);
      memcpy(outSsid, body + 2, n);
      outSsid[n] = '\0';
      return true;
    }
    body += elen + 2; len -= elen + 2;
  }
  return false;
}

// Returns:
//   1  = wildcard SSID IE found (tag 0, length 0)  → Flock-style probe
//   0  = SSID IE found, non-zero length            → directed probe, not ours
//  -1  = no SSID IE found at all                   → caller should retry with
//                                                    FCS-stripped length, then bail
static int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    uint8_t id   = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len  -= elen + 2;
  }
  return -1;
}

// --- IE fingerprint matcher (from colonelpanichacks PR #45) ---
// Strict ordered walk of probe-request IE TLVs against the known Flock
// primary probe IE sequence.  This is the highest-precision detection
// signal: wildcard SSID + ordered Information Elements including vendor
// tags that match Flock's Liteon and WPA vendor payloads.

enum class FyIeExpect : uint8_t { TagOnly, VendorPayload };

struct FyExpectedIe {
  uint8_t        tag;
  FyIeExpect     kind;
  uint8_t        vendorLen;
  const uint8_t* vendor;
};

static const uint8_t FLOCK_VENDOR_LITEON[] = {0x50, 0x6f, 0x9a, 0x16, 0x03, 0x01, 0x03};
static const uint8_t FLOCK_VENDOR_WPA[]    = {0x00, 0x50, 0xf2, 0x08, 0x00, 0x00, 0x00};

static const FyExpectedIe FLOCK_PRIMARY_PROBE_IES[] = {
  {0,   FyIeExpect::TagOnly,        0, nullptr},
  {2,   FyIeExpect::TagOnly,        0, nullptr},
  {12,  FyIeExpect::TagOnly,        0, nullptr},
  {127, FyIeExpect::TagOnly,        0, nullptr},
  {221, FyIeExpect::VendorPayload, 7, FLOCK_VENDOR_LITEON},
  {45,  FyIeExpect::TagOnly,        0, nullptr},
  {191, FyIeExpect::TagOnly,        0, nullptr},
  {221, FyIeExpect::VendorPayload, 7, FLOCK_VENDOR_WPA},
};

static inline bool IRAM_ATTR tlvFits(int i, int elen, int len) {
  return i + 2 + elen <= len;
}

static bool IRAM_ATTR matchPrimaryFlockProbeIes(const uint8_t* body, int len) {
  if (!body || len < 2) return false;
  int i = 0;
  for (size_t n = 0; n < sizeof(FLOCK_PRIMARY_PROBE_IES) / sizeof(FLOCK_PRIMARY_PROBE_IES[0]); n++) {
    const FyExpectedIe& exp = FLOCK_PRIMARY_PROBE_IES[n];
    if (i + 2 > len) return false;
    uint8_t id = body[i];
    int elen = (int)body[i + 1];
    if (!tlvFits(i, elen, len)) return false;

    if (exp.tag == 0) {
      if (id != 0 || elen != 0) return false;
      i += 2;
      continue;
    }
    if (exp.kind == FyIeExpect::TagOnly) {
      if (id != exp.tag) return false;
      i += 2 + elen;
      continue;
    }
    if (id != exp.tag || elen != (int)exp.vendorLen) return false;
    if (memcmp(body + i + 2, exp.vendor, exp.vendorLen) != 0) return false;
    i += 2 + elen;
  }
  return i == len;
}

// Match primary signature; retry with 4-byte FCS stripped when sig_len includes trailer.
static bool IRAM_ATTR probeBodyMatchesPrimary(const uint8_t* body, int bodyLen) {
  if (!body || bodyLen < 2) return false;
  if (matchPrimaryFlockProbeIes(body, bodyLen)) return true;
  if (bodyLen > 4 && matchPrimaryFlockProbeIes(body, bodyLen - 4)) return true;
  return false;
}

// --- Confidence scoring ---
// Returns a string label for the detection confidence level.
// Ranking: wildcard_probe_ie_sig > wildcard_probe > ssid > oui_addr2 > oui_addr1 > hidden_ssid
static const char* alertConfidence(AlertType t) {
  switch (t) {
    case ALERT_WILDCARD_PROBE_IE_SIG: return "high";
    case ALERT_WILDCARD_PROBE:        return "high";
    case ALERT_SSID:                  return "medium";
    case ALERT_OUI_ADDR2:             return "medium";
    case ALERT_OUI_ADDR1:             return "low";
    case ALERT_HIDDEN_SSID:           return "low";
    case ALERT_OUI_ADDR3:             return "low";
    default:                           return "unknown";
  }
}


static void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || sniffingStopped) return;

#if PROCESS_MGMT_FRAMES && PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif PROCESS_MGMT_FRAMES
  if (type != WIFI_PKT_MGMT) return;
#elif PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_DATA) return;
#else
  return;  // nothing configured to process
#endif

  wifi_promiscuous_pkt_t*      pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;
  wifi_ieee80211_mac_hdr_t*    hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi;

  wifiRxFrames++;
  if (type == WIFI_PKT_MGMT) wifiRxMgmtFrames++;
  if (type == WIFI_PKT_DATA) wifiRxDataFrames++;

  if (rssi < RSSI_MIN) return;

  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;  // actual rx channel from driver
  const bool isMgmt = (type == WIFI_PKT_MGMT);
  uint8_t ftype = 0;
  uint8_t subtype = 0;
  if (isMgmt) {
    uint8_t fc0 = hdr->frame_ctrl & 0xFF;
    ftype = (fc0 >> 2) & 0x03;
    subtype = (fc0 >> 4) & 0x0F;
  }

  // --- OUI check: addr2 (transmitter/source) ---
  //
  // For mgmt Probe Requests (type=0 subtype=4) from a matched OUI:
  //   1. Try strict IE fingerprint match (wildcard SSID + ordered IE TLVs)
  //      → ALERT_WILDCARD_PROBE_IE_SIG (highest confidence)
  //   2. Fall back to wildcard-SSID-only match (original DeFlockJoplin sig)
  //      → ALERT_WILDCARD_PROBE (high confidence)
  //   3. Non-probe frames from the same OUI → ALERT_OUI_ADDR2 (medium)
  //
  // For Beacons/Probe Responses from a matched OUI with hidden SSID:
  //   → ALERT_HIDDEN_SSID (low)
  if (matchOuiRaw(hdr->addr2)) {
    bool emitted = false;
    if (isMgmt && ftype == 0) {
      if (ftype == 0 && subtype == 4) {                        // Probe Request
        int sigLen  = (int)pkt->rx_ctrl.sig_len;
        int bodyLen = sigLen - (int)sizeof(wifi_ieee80211_mac_hdr_t);
        const uint8_t* body = pkt->payload + sizeof(wifi_ieee80211_mac_hdr_t);
        // Try strict IE fingerprint first
        if (probeBodyMatchesPrimary(body, bodyLen)) {
          enqueueAlert(ALERT_WILDCARD_PROBE_IE_SIG, hdr->addr2, rssi, ch,
                       nullptr, "probe_req");
          emitted = true;
        } else {
          // Fall back to wildcard-SSID-only match
          int r = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;
          if (r == -1 && bodyLen > 4) r = isWildcardProbeIE(body, bodyLen - 4);
          if (r == 1) {
            enqueueAlert(ALERT_WILDCARD_PROBE, hdr->addr2, rssi, ch,
                         nullptr, "probe_req");
            emitted = true;
          }
        }
      } else if (subtype == 8 || subtype == 5) {                // Beacon / Probe Response
        int sigLen = (int)pkt->rx_ctrl.sig_len;
        int off = (int)sizeof(wifi_ieee80211_mac_hdr_t) + 12;
        int bodyLen = sigLen - off;
        const uint8_t* body = pkt->payload + off;
        int r = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;
        if (r == -1 && bodyLen > 4) r = isWildcardProbeIE(body, bodyLen - 4);
        if (r == 1) {
          enqueueAlert(ALERT_HIDDEN_SSID, hdr->addr2, rssi, ch,
                       nullptr, subtype == 8 ? "hidden_beacon" : "hidden_probe");
          emitted = true;
        }
      }
    }
    if (!emitted) {
      enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, rssi, ch, nullptr, "addr2");
    }
  }

#if CHECK_ADDR1
  // addr1 (receiver/destination): catches Flock STAs that appear only as the
  // dst of probe responses and data frames — never transmitting in the capture
  // window due to their burst-sleep duty cycle. Multicast guard is mandatory
  // here since addr1 is broadcast (ff:ff:ff:ff:ff:ff) in beacons/broadcasts.
  if (!isMulticast(hdr->addr1) && matchOuiRaw(hdr->addr1)) {
    enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, rssi, ch, nullptr, "addr1");
  }
#endif

#if CHECK_ADDR3
  // addr3 fallback: catches cases where addr2 is randomised but addr3
  // carries the real BSSID OUI (management frames only).
  if (type == WIFI_PKT_MGMT && matchOuiRaw(hdr->addr3)) {
    enqueueAlert(ALERT_OUI_ADDR3, hdr->addr3, rssi, ch, nullptr, "addr3");
  }
#endif

#if ENABLE_SSID_MATCH
  if (isMgmt) {
    if (ftype == 0) {
      int sigLen = pkt->rx_ctrl.sig_len - 4;  // strip 4-byte FCS
      if (sigLen < (int)sizeof(wifi_ieee80211_mac_hdr_t)) return;

      const uint8_t* mgmtBody    = nullptr;
      int            mgmtBodyLen = 0;
      const char*    frameKind   = nullptr;

      if (subtype == 8 || subtype == 5) {
        // Beacon / Probe Response: fixed params = 12 bytes after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t) + 12;
        if (sigLen > off) {
          frameKind   = (subtype == 8) ? "beacon" : "probe_resp";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      } else if (subtype == 4) {
        // Probe Request: IEs follow directly after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t);
        if (sigLen > off) {
          frameKind   = "probe_req";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      }

      if (mgmtBody && mgmtBodyLen > 0) {
        char ssid[33] = {0};
        if (extractSsidFromMgmtBody(mgmtBody, mgmtBodyLen, ssid, sizeof(ssid))) {
          if (matchSsidKeyword(ssid)) {
            enqueueAlert(ALERT_SSID, hdr->addr2, rssi, ch, ssid, frameKind);
          }
        }
      }
    }
  }
#endif
}

// ============================================================
// DRAIN QUEUE — called from loop(), safe to Serial.print here
// ============================================================

static void drainAlertQueue() {
  while (true) {
    portENTER_CRITICAL(&queueMux);
    if (alertTail == alertHead) { portEXIT_CRITICAL(&queueMux); break; }
    AlertEntry e;
    memcpy(&e, (const void*)&alertQueue[alertTail], sizeof(AlertEntry));
    alertTail = (alertTail + 1) % ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&queueMux);

    char macStr[18];
    macToStr(e.mac, macStr, sizeof(macStr));
    const char* method = alertTypeToMethod(e.type);

    // Always update the on-device detection table (survives reboot via SPIFFS).
    // chirpWorthy = true for brand-new MACs AND for MACs rediscovered after
    // REDISCOVER_MS of silence (drove away and came back).
    bool chirpWorthy = false;
    int idx = fyAddDetection(macStr, method, e.rssi, e.channel,
                             (e.type == ALERT_SSID) ? e.ssid : nullptr,
                             &chirpWorthy);

    // Refresh the global "still around" timer for the heartbeat tick.
    // Done unconditionally so a device counts as active even when serial is
    // rate-limited (still audible via heartbeat, just quieter on the wire).
    fyLastTargetSeen = millis();

    // Track per-method hit counts
    if ((uint8_t)e.type < 8) methodCounts[(uint8_t)e.type]++;

    // Serial-rate-limit: suppress emit/beep/flash within ALERT_COOLDOWN_MS.
    if (shouldSuppressDuplicate(macStr)) continue;

    // Human-readable line (for serial terminal / mirror).
    char oui[9];
    ouiFromMac(e.mac, oui, sizeof(oui));
    if (e.type == ALERT_SSID) {
      dualPrintf("[flockyou] DETECT-SSID type=%s mac=%s ssid=\"%s\" rssi=%d ch=%u count=%d\n",
                 e.frameKind, macStr, e.ssid, e.rssi, e.channel,
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    } else {
      dualPrintf("[flockyou] DETECT-OUI mac=%s oui=%s rssi=%d ch=%u addr=%s count=%d\n",
                 macStr, oui, e.rssi, e.channel,
                 e.frameKind[0] ? e.frameKind : "addr2",
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    }

#if FY_UI_BUILD
    cydRecordDetection(macStr, oui, method, e.rssi, e.channel,
                       (idx >= 0) ? fyDet[idx].count : 0);
#endif

    // Flask-compatible JSON line (parsed by api/flockyou.py over USB CDC).
    emitDetectionJSON(macStr, method, e.rssi, e.channel,
                      (e.type == ALERT_SSID) ? e.ssid : "",
                      alertConfidence(e.type));

    // Audio feedback:
    //   - NEW MAC  → two fast ascending beeps (clearly distinct sound)
    //   - REPEAT   → silent; the heartbeat tick covers continued presence
    // LED flashes on every emitted detection either way.
    if (chirpWorthy) {
      newDetectChirp();
      // Reset the heartbeat phase so the first follow-up beep lands
      // HB_BEEP_INTERVAL_MS after the initial chirp, not mid-window.
      fyLastHeartbeatAt = millis();
    }
    ledFlash(LED_FLASH_MS);

#if STOP_ON_OUI_HIT
    if (e.type != ALERT_SSID) stopSniffing("OUI hit");
#endif
#if STOP_ON_SSID_HIT
    if (e.type == ALERT_SSID) stopSniffing("SSID hit");
#endif
  }
}

// ============================================================
// AUTOSAVE
// ============================================================

static void autosaveTick() {
  if (!fySpiffsReady || !fyDirty) return;
  if (millis() - fyLastSaveAt < AUTOSAVE_INTERVAL_MS) return;
  fySaveSession();
}

// Heartbeat beep while at least one target was seen in the last
// HB_DEVICE_ACTIVE_MS. Fires HB_BEEP_INTERVAL_MS apart.
static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;                           // never seen one
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;    // gone silent
  if (now - fyLastHeartbeatAt < HB_BEEP_INTERVAL_MS) return;   // too soon
  heartbeatBeep();
  fyLastHeartbeatAt = now;
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
#if !CYD_BUILD
  // Crucial for USB-optional operation: without this, Serial.write() will
  // block indefinitely on an ESP32-S3 USB-CDC port when no host is attached.
  // Applies to every non-CYD build (xiao headless AND CrowPanel — both are
  // S3 native-USB builds); the classic-ESP32 CYD never uses USB-CDC.
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);

#if MIRROR_SERIAL
  Serial1.begin(MIRROR_BAUD, SERIAL_8N1, -1, MIRROR_TX_PIN);  // TX-only on GPIO43
#endif

#if USE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif

#if USE_LED
  pinMode(LED_PIN, OUTPUT);
  ledSet(false);
#endif

  startupBeep();
#if USE_LED
  ledFlash(200);
#endif

  precompileOuis();
  memset(dedupeTable, 0, sizeof(dedupeTable));

  // SPIFFS — format on first boot if missing. Non-fatal if it fails.
  // Runs before cydInit() so a persisted FYROTATE orientation can be
  // applied before the display is first configured.
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[flockyou] SPIFFS ready");
    fyPromotePrevSession();
  } else {
    dualPrintln("[flockyou] SPIFFS init FAILED — running without persistence");
  }

#if FY_UI_BUILD
  // Apply the FYROTATE-persisted orientation (compiled CYD_TFT_ROTATION
  // default when absent/invalid) before cydInit()'s first setRotation().
  cydTftRotation = fyLoadRotation();
  cydInit();
  bleFlockLastScanMs = 0;
#endif

  WiFi.mode(WIFI_MODE_NULL);
  // Optimized WiFi init config — disables AMPDU, CSI, and NVS to reduce
  // overhead and free resources for promiscuous mode sniffing.
  // Modeled after the ESP32 Marauder's cfg2.
  static wifi_init_config_t fyWifiCfg = {
    .event_handler = &esp_event_send_internal,
    .osi_funcs = &g_wifi_osi_funcs,
    .wpa_crypto_funcs = g_wifi_default_wpa_crypto_funcs,
    .static_rx_buf_num = 6,
    .dynamic_rx_buf_num = 6,
    .tx_buf_type = 0,
    .static_tx_buf_num = 1,
    .dynamic_tx_buf_num = WIFI_DYNAMIC_TX_BUFFER_NUM,
    .cache_tx_buf_num = 0,
    .csi_enable = false,
    .ampdu_rx_enable = false,
    .ampdu_tx_enable = false,
    .amsdu_tx_enable = false,
    .nvs_enable = false,
    .nano_enable = WIFI_NANO_FORMAT_ENABLED,
    .rx_ba_win = 6,
    .wifi_task_core_id = WIFI_TASK_CORE_ID,
    .beacon_max_len = 752,
    .mgmt_sbuf_num = 8,
    .feature_caps = g_wifi_feature_caps,
    .sta_disconnected_pm = WIFI_STA_DISCONNECTED_PM_ENABLED,
    .espnow_max_encrypt_num = 0,
    .magic = WIFI_INIT_CONFIG_MAGIC
  };
  esp_wifi_init(&fyWifiCfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();

  applyInitialChannel();

  wifi_promiscuous_filter_t filt = {
    .filter_mask = 0
#if PROCESS_MGMT_FRAMES
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if PROCESS_DATA_FRAMES
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
  };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
  esp_wifi_set_promiscuous(true);

  dualPrintln("[flockyou] merged WiFi detector started");
  dualPrintf("[flockyou] mode=%s dwell_ms=%u start_channel=%u rssi_min=%d spiffs=%d\n",
                channelModeName(), CHANNEL_DWELL_MS, currentChannel,
                RSSI_MIN, fySpiffsReady ? 1 : 0);

  lastHeartbeat = millis();
  fyLastSaveAt  = millis();
}

void loop() {
#if FY_UI_BUILD
  cydSerialTick();
  cydBleDrainCommands();
  cydBleFlockTick();
  cydButtonTick();
  cydTouchTick();
  if (cydBleConnectionChanged) {
    cydBleConnectionChanged = false;
    cydDrawUi(true);
    cydEmitPairStatus();
  }
  cydDrawUi(false);
#endif
  updateChannelMode();
  drainAlertQueue();   // Serial.printf happens here, not in callback
  autosaveTick();      // periodic SPIFFS write if dirty
  heartbeatTick();     // audible beep-pair while a target is still in range
  ledTick();           // turn off LED after LED_FLASH_MS
  printHeartbeat();
  delay(1);
}
