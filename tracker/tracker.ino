// =============================================================================
//  tracker.ino — IoT Animal Tracking Collar Firmware
//  Target: Seeed Studio XIAO ESP32-S3
//
//  BOARD SELECTION IN ARDUINO IDE:
//    Tools → Board → "XIAO_ESP32S3"
//    Tools → USB CDC On Boot → "Enabled"   ← required for Serial Monitor
//
//  PIN ASSIGNMENTS (XIAO ESP32-S3 breadboard labels → actual GPIO):
//    I2C (BNO055):  SDA = D4 / GPIO 5   |   SCL = D5 / GPIO 6
//    GPS (NEO-M8N): RX  = D7 / GPIO 44  |   TX  = D6 / GPIO 43
//
//  WIRING TABLE:
//    XIAO Pin  │ XIAO Label │ GPIO  │ Connected To
//    ──────────┼────────────┼───────┼─────────────────────
//    SDA       │ D4         │ 5     │ BNO055 SDA
//    SCL       │ D5         │ 6     │ BNO055 SCL
//    RX (GPS)  │ D7         │ 44    │ NEO-M8N TX wire
//    TX (GPS)  │ D6         │ 43    │ NEO-M8N RX wire
//    3.3V      │ 3V3        │ —     │ BNO055 VIN, GPS VCC
//    GND       │ GND        │ —     │ BNO055 GND, GPS GND
//
//  ⚠  OTHER ESP32 VARIANTS:
//    ESP32-WROOM/WROVER: change SDA=21, SCL=22, GPS_RX=16, GPS_TX=17
//    ESP32-C3: use HardwareSerial(1) with any free pins; no UART2.
//    ESP32-S3 DevKit: default I2C is SDA=8, SCL=9 — adjust as needed.
//
//  Dependencies (install via Arduino Library Manager):
//    • TinyGPS++          (Mikal Hart)
//    • Adafruit BNO055    (Adafruit)
//    • Adafruit Unified Sensor
//    • ArduinoJson        v7+
//    • WiFiMulti          (bundled with ESP32 Arduino core)
//
//  Secrets: copy secrets.h.template → secrets.h and fill in credentials.
//  secrets.h is gitignored — NEVER commit it.
// =============================================================================

#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_BNO055.h>
#include <Adafruit_Sensor.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <esp_sleep.h>
#include "secrets.h"
#include "motion_filter.h"

// =============================================================================
//  PIN DEFINITIONS — XIAO ESP32-S3 (change here for other ESP32 variants)
// =============================================================================
// GPS NEO-M8N on Serial1 (XIAO UART1)
constexpr int GPS_RX_PIN = 44;  // XIAO D7 → connects to NEO-M8N TX wire
constexpr int GPS_TX_PIN = 43;  // XIAO D6 → connects to NEO-M8N RX wire
// BNO055 on I2C
constexpr int SDA_PIN    = 5;   // XIAO D4
constexpr int SCL_PIN    = 6;   // XIAO D5

// =============================================================================
//  TIMING & QUALITY THRESHOLDS
// =============================================================================
constexpr uint32_t FRESH_MS        = 3000;   // max GPS fix age (ms)
constexpr bool     DEMO_MODE       = true;   // keep awake for demonstrations
constexpr uint32_t UPLOAD_MS       = DEMO_MODE ? 1000 : 2000;
constexpr uint32_t SENSOR_MS       = 50;     // IMU polling interval (ms)
constexpr double   MAX_HDOP        = 5.0;    // max acceptable HDOP
constexpr uint32_t MIN_SATELLITES  = 4;      // min satellites for good fix
constexpr double   MAX_SPEED_MPS   = 60.0;   // sanity cap (m/s) for jump check
constexpr double   JUMP_SLACK_M    = 30.0;   // hard speed-based jump threshold
constexpr double   RESTING_JUMP_M  = 8.0;    // resting-state micro-jump threshold
constexpr uint32_t REACQUIRE_MS    = 15000;  // time before treating fix as re-acquired

// Deep-sleep parameters
constexpr uint32_t INACTIVITY_MS   = 240000UL;              // 4 min non-moving → sleep
constexpr uint64_t SLEEP_US        = 10ULL * 60 * 1000000;  // 10 min deep sleep

// Activity classification thresholds
constexpr float    GRAZING_ACCEL   = 0.5f;   // m/s² — above this AND pitch ok → Grazing
constexpr float    GRAZING_PITCH   = 25.0f;  // degrees — abs(pitch) > this → Grazing candidate

// Wi-Fi AP scan threshold: only include APs with RSSI above this
constexpr int      AP_RSSI_THRESH  = -75;

// =============================================================================
//  STRUCTS
// =============================================================================

struct WifiAP {
  char bssid[18];   // "AA:BB:CC:DD:EE:FF\0"
  int  rssi;
};

struct Snapshot {
  bool   fixOK;
  bool   sensorOK;
  double lat, lon;
  double epoch;      // GPS-derived or NTP Unix timestamp
  double hdop;
  double speed;      // km/h
  double altitude;   // metres
  int    satellites;
  float  ax, ay, az; // accelerometer (m/s²)
  float  gx, gy, gz; // gyroscope (rad/s)
  char   reason[16]; // "waiting"|"fresh"|"weak_fix"|"jump_rejected"|"reacquired"|"stale"
  char   activity[12]; // "Resting"|"Grazing"|"Moving"
  WifiAP aps[3];
  int    apCount;    // 0–3 valid entries in aps[]
};

// =============================================================================
//  GLOBALS
// =============================================================================

TinyGPSPlus        gps;
HardwareSerial&    GPS_Serial = Serial1;  // XIAO ESP32-S3: Serial1 on GPIO 44/43

// BNO055 pointer dynamically allocated in startBNO055() to avoid object copy corruption
Adafruit_BNO055   *bno = nullptr;
WiFiMulti          wifiMulti;

QueueHandle_t      uploads  = nullptr;

bool               sensorOK = false;
bool               haveFix  = false;

double             acceptedLat   = 0.0;
double             acceptedLon   = 0.0;
double             acceptedEpoch = 0.0;
uint32_t           acceptedAt    = 0;
const char        *assessment    = "waiting";

// Current activity string (shared between loop() and networkTask via Snapshot)
char               currentActivity[12] = "Moving";
MotionFilter motionFilter;
bool stationaryAnchorValid = false;
double stationaryAnchorLat = 0.0, stationaryAnchorLon = 0.0;
bool acceptedSpeedValid = false;
double acceptedSpeedMps = 0.0;

// Inactivity tracking for deep-sleep
uint32_t           inactiveStart = 0;
bool               inactivityTracking = false;

// IMU event data (updated in loop, copied into Snapshot)
sensors_event_t    accelEvent{};
sensors_event_t    gyroEvent{};

// Wi-Fi scan state (non-blocking async scan)
int                scanResult  = WIFI_SCAN_RUNNING;
bool               scanStarted = false;
uint32_t           lastScanAt  = 0;
constexpr uint32_t SCAN_INTERVAL_MS = 10000; // re-scan every 10 s

// =============================================================================
//  GPS EPOCH CALCULATION
//  Converts GPS date/time fields into a Unix epoch (double, UTC).
// =============================================================================
double gpsEpoch() {
  int y = gps.date.year();
  int m = gps.date.month();
  int d = gps.date.day();

  if (y < 2020 || y > 2099 ||
      m < 1    || m > 12   ||
      d < 1    || d > 31) {
    return 0;
  }

  // Civil-date-to-epoch using Howard Hinnant's algorithm
  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

  int64_t days = (int64_t)era * 146097 + doe - 719468;

  return double(days * 86400LL +
    gps.time.hour()   * 3600LL +
    gps.time.minute() * 60LL   +
    gps.time.second()) +
    gps.time.centisecond() / 100.0;
}

// =============================================================================
//  ACTIVITY CLASSIFICATION
//  Uses gravity-free acceleration, gyro stillness, prior GPS speed, and pitch.
//  Returns a static string: "Resting", "Grazing", or "Moving".
// =============================================================================
const char* classifyActivity() {
  if (!sensorOK) return "Moving"; // default safe assumption

  float ax = accelEvent.acceleration.x;
  float ay = accelEvent.acceleration.y;
  float az = accelEvent.acceleration.z;

  float accelMag = sqrtf(ax * ax + ay * ay + az * az);

  if (motionFilter.stationary(millis()) && (!acceptedSpeedValid || acceptedSpeedMps <= 0.5)) {
    return "Resting";
  }

  // Read Euler angles for pitch from BNO055
  imu::Vector<3> euler = bno->getVector(Adafruit_BNO055::VECTOR_EULER);
  // BNO055 Euler: x=heading(yaw), y=roll, z=pitch
  float pitch = euler.z();

  if (fabsf(pitch) > GRAZING_PITCH && accelMag > GRAZING_ACCEL) {
    return "Grazing";
  }

  return "Moving";
}

// =============================================================================
//  consumeFix()
//  Validates and applies a new GPS reading with quality and jump checks.
//  Called from loop() on every decoded NMEA sentence.
// =============================================================================
void consumeFix() {
  if (!gps.location.isUpdated()) return;

  uint32_t age = gps.location.age();
  double lat   = gps.location.lat();
  double lon   = gps.location.lng();

  // Basic validity and freshness
  if (!gps.location.isValid() ||
      age > FRESH_MS          ||
      !gps.date.isValid()     ||
      !gps.time.isValid()     ||
      gps.date.age() > FRESH_MS ||
      gps.time.age() > FRESH_MS) {
    return;
  }

  double hdop = gps.hdop.isValid() ? gps.hdop.hdop() : NAN;
  int    sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;

  Serial.printf("RAW,%.2f,%.8f,%.8f,%.2f,%d\n",
                gpsEpoch(), lat, lon, hdop, sats);

  // Quality gate: HDOP and satellite count
  bool quality =
    gps.hdop.isValid()       &&
    gps.hdop.age() < FRESH_MS &&
    gps.satellites.isValid() &&
    gps.satellites.age() < FRESH_MS &&
    hdop > 0                 &&
    hdop <= MAX_HDOP         &&
    sats >= (int)MIN_SATELLITES;

  if (!quality) {
    assessment = "weak_fix";
    return;
  }

  double epoch = gpsEpoch();
  if (epoch == 0 || (haveFix && epoch <= acceptedEpoch)) {
    return;
  }

  uint32_t now = millis();
  bool reacquire = haveFix && uint32_t(now - acceptedAt) >= REACQUIRE_MS;

  // Protect a stationary anchor even when activity is not labelled Resting,
  // and even after a GPS outage. Candidate GPS speed cannot override quiet IMU
  // evidence plus the previously accepted near-zero speed.
  bool protect = sensorOK && haveFix &&
    motionFilter.protectPosition(now, acceptedSpeedValid, acceptedSpeedMps);
  if (!protect) stationaryAnchorValid = false;
  if (protect) {
    if (!stationaryAnchorValid) {
      stationaryAnchorLat = acceptedLat;
      stationaryAnchorLon = acceptedLon;
      stationaryAnchorValid = true;
    }
    double drift = TinyGPSPlus::distanceBetween(
      stationaryAnchorLat, stationaryAnchorLon, lat, lon);
    if (drift > RESTING_JUMP_M) {
      assessment = "jump_rejected";
      return;
    }
    // Do not let small repeated GPS errors slowly walk a stationary position.
    lat = stationaryAnchorLat;
    lon = stationaryAnchorLon;
  }

  if (haveFix && !reacquire) {
    double elapsed  = epoch - acceptedEpoch;
    double distance = TinyGPSPlus::distanceBetween(
      acceptedLat, acceptedLon, lat, lon
    );

    // Speed-based large jump check
    if (distance > JUMP_SLACK_M + MAX_SPEED_MPS * elapsed) {
      assessment = "jump_rejected";
      return;
    }
  }

  // Accept the fix
  acceptedSpeedValid = gps.speed.isValid() && gps.speed.age() < FRESH_MS;
  acceptedSpeedMps = protect ? 0.0 : (acceptedSpeedValid ? gps.speed.mps() : 0.0);
  acceptedLat   = lat;
  acceptedLon   = lon;
  acceptedEpoch = epoch;
  acceptedAt    = now - age;
  haveFix       = true;
  assessment    = reacquire ? "reacquired" : "fresh";
}

// =============================================================================
//  BNO055 HELPERS
// =============================================================================
bool startBNO055() {
  // Try I2C address 0x28 first (7Semi BNO055 default), then 0x29 as fallback.
  // Wire must already be initialised before calling this.
  static const uint8_t addrs[] = {0x28, 0x29};
  for (uint8_t addr : addrs) {
    if (bno) {
      delete bno;
      bno = nullptr;
    }
    bno = new Adafruit_BNO055(55, addr, &Wire);
    if (bno && bno->begin()) {
      Serial.printf("BNO055 found at I2C address 0x%02X\n", addr);
      delay(1000);
      bno->setExtCrystalUse(true);
      return true;
    }
  }
  return false;
}

bool readBNO055() {
  if (!sensorOK || !bno) return false;

  // Gravity-free acceleration: a stationary sensor should read near zero.
  bool ok1 = bno->getEvent(&accelEvent, Adafruit_BNO055::VECTOR_LINEARACCEL);
  bool ok2 = bno->getEvent(&gyroEvent,  Adafruit_BNO055::VECTOR_GYROSCOPE);

  if (!ok1 || !ok2) return false;

  return
    isfinite(accelEvent.acceleration.x) &&
    isfinite(accelEvent.acceleration.y) &&
    isfinite(accelEvent.acceleration.z) &&
    isfinite(gyroEvent.gyro.x)          &&
    isfinite(gyroEvent.gyro.y)          &&
    isfinite(gyroEvent.gyro.z);
}

// =============================================================================
//  Wi-Fi AP SCAN HELPERS (non-blocking async)
//  Populates top-3 APs by RSSI into dest[].  Returns count found.
//  Only includes APs with RSSI > AP_RSSI_THRESH.
// =============================================================================
int collectTopAPs(WifiAP dest[3]) {
  int n = WiFi.scanComplete();
  if (n <= 0) return 0; // scan still running or no APs

  // Build a list of indices sorted by RSSI (descending) — simple selection
  int indices[n];
  for (int i = 0; i < n; i++) indices[i] = i;

  // Bubble-sort by RSSI descending (n is at most ~20 in typical environments)
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
        int tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
      }
    }
  }

  int count = 0;
  for (int i = 0; i < n && count < 3; i++) {
    int idx  = indices[i];
    int rssi = WiFi.RSSI(idx);
    if (rssi <= AP_RSSI_THRESH) break; // sorted descending; rest are weaker
    dest[count].rssi = rssi;
    strlcpy(dest[count].bssid, WiFi.BSSIDstr(idx).c_str(), sizeof(dest[count].bssid));
    count++;
  }

  // Delete scan results to free memory and allow next scan
  WiFi.scanDelete();
  return count;
}

// =============================================================================
//  NETWORK / UPLOADER TASK  (FreeRTOS, runs on core 0)
//  Receives Snapshot from queue, builds JSON, POSTs to Flask server.
// =============================================================================
void networkTask(void *parameter) {
  // --- Wi-Fi setup with WiFiMulti ---
  WiFi.mode(WIFI_STA);

  // Primary campus IoT network; password from secrets.h
  wifiMulti.addAP(SSID1, SSID1_PASSWORD);

  // Backup mobile hotspot; SSID + password from secrets.h
  wifiMulti.addAP(SSID2, SSID2_PASSWORD);

  // Print MAC address (required for IITR ICC MAC-whitelisting).
  // Repeat every 10 s until connected so it's easy to copy from Serial Monitor.
  Serial.print("MAC Address: "); Serial.println(WiFi.macAddress());
  Serial.println("Waiting for Wi-Fi... (MAC will repeat every 10 s until connected)");
  uint32_t lastMacPrint = millis();
  while (wifiMulti.run() != WL_CONNECTED) {
    if (millis() - lastMacPrint >= 10000) {
      lastMacPrint = millis();
      Serial.print("MAC Address: "); Serial.println(WiFi.macAddress());
      Serial.printf("Still connecting to Wi-Fi (tried: %s / %s)...\n", SSID1, SSID2);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  Serial.printf("Wi-Fi connected → SSID: %s  IP: %s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  // NTP time sync (UTC)
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  uint32_t backoffMS = 2000;

  for (;;) {
    Snapshot s;

    // Block up to 1 second waiting for a snapshot from loop()
    if (xQueueReceive(uploads, &s, pdMS_TO_TICKS(1000)) != pdTRUE) {
      // No snapshot — keep WiFiMulti alive
      wifiMulti.run();
      continue;
    }

    // Ensure Wi-Fi is connected (WiFiMulti picks the best available AP)
    if (wifiMulti.run() != WL_CONNECTED) {
      Serial.println("Wi-Fi not connected — skipping upload.");
      continue;
    }

    // ---- Security check ----
    String url(API_URL);
    bool   secure = url.startsWith("https://");

    if (!secure && !(ALLOW_LAN_HTTP && url.startsWith("http://"))) {
      Serial.println("Blocked: HTTPS required (set ALLOW_LAN_HTTP=true for dev).");
      continue;
    }

    if (secure &&
        (time(nullptr) < 1704067200 ||
         !strstr(ROOT_CA, "BEGIN CERTIFICATE"))) {
      Serial.println("HTTPS waiting: NTP not synced or ROOT_CA missing.");
      continue;
    }

    // ---- Stale-fix check in uploader ----
    if (s.fixOK &&
        time(nullptr) >= 1704067200 &&
        difftime(time(nullptr), (time_t)s.epoch) > 10) {
      s.fixOK = false;
      strlcpy(s.reason, "stale", sizeof(s.reason));
    }

    // ---- Build JSON payload (Flask schema) ----
    JsonDocument doc;

    doc["fix_ok"]   = s.fixOK;
    doc["reason"]   = s.reason;
    doc["mpu_ok"]   = s.sensorOK;

    // Always include lat/lon/fix_epoch so Flask can store last known position
    doc["lat"]       = s.lat;
    doc["lon"]       = s.lon;
    doc["fix_epoch"] = s.epoch;

    if (s.satellites >= 0)  doc["satellites"] = s.satellites;
    if (isfinite(s.hdop))   doc["hdop"]       = s.hdop;
    if (isfinite(s.speed))  doc["speed_kmph"] = s.speed;
    if (isfinite(s.altitude)) doc["altitude_m"] = s.altitude;

    if (s.sensorOK) {
      doc["ax"] = s.ax;
      doc["ay"] = s.ay;
      doc["az"] = s.az;
      doc["gx"] = s.gx;
      doc["gy"] = s.gy;
      doc["gz"] = s.gz;
    }

    doc["activity"] = s.activity;

    // ---- Wi-Fi APs (omit field entirely if no AP passes threshold) ----
    if (s.apCount > 0) {
      JsonArray aps = doc["wifi_aps"].to<JsonArray>();
      for (int i = 0; i < s.apCount; i++) {
        JsonObject ap = aps.add<JsonObject>();
        ap["bssid"] = s.aps[i].bssid;
        ap["rssi"]  = s.aps[i].rssi;
      }
    }

    String payload;
    serializeJson(doc, payload);

    // ---- HTTP(S) POST ----
    WiFiClient       plain;
    WiFiClientSecure tls;
    HTTPClient       http;

    if (secure) {
      tls.setCACert(ROOT_CA);
      tls.setHandshakeTimeout(8);
    }

    http.setConnectTimeout(8000);
    http.setTimeout(8000);

    bool begun = secure
      ? http.begin(tls, url)
      : http.begin(plain, url);

    int status = -1;

    if (begun) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);

      status = http.POST(payload);

      String response = http.getString();
      Serial.printf("Upload HTTP %d → %s\n", status,
                    (status >= 200 && status < 300) ? "OK" : response.c_str());

      http.end();
    } else {
      Serial.println("HTTP begin() failed.");
    }

    // Backoff on failure
    if (status >= 200 && status < 300) {
      backoffMS = 2000;
    } else {
      vTaskDelay(pdMS_TO_TICKS(backoffMS));
      backoffMS = min(backoffMS * 2U, (uint32_t)30000U);
    }
  }
}

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  // USB CDC Serial — works on XIAO ESP32-S3 when "USB CDC On Boot: Enabled"
  Serial.begin(115200);
  delay(1500);  // give USB CDC time to enumerate before printing

  Serial.println("\n=== Animal Tracking Collar — XIAO ESP32-S3 ===");

  // Initialize Wi-Fi and print MAC address immediately before any sensor/bus initialization
  WiFi.mode(WIFI_STA);
  Serial.println("MAC Address: " + WiFi.macAddress());
  Serial.println("(Register the MAC above with IITR ICC for campus Wi-Fi access.)");

  // ── GPS: NEO-M8N on Serial1 (D7=RX GPIO44, D6=TX GPIO43) ──────────────────
  GPS_Serial.setRxBufferSize(2048);
  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("GPS Serial1 started: RX=GPIO%d (D7), TX=GPIO%d (D6)\n",
                GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS wiring check: waiting 3 s for raw NMEA bytes...");
  {
    uint32_t t0 = millis();
    uint32_t nmeaBytes = 0;
    while (millis() - t0 < 3000) {
      while (GPS_Serial.available()) { GPS_Serial.read(); nmeaBytes++; }
    }
    if (nmeaBytes > 0) {
      Serial.printf("GPS OK — received %lu raw bytes in 3 s. TX/RX wiring good.\n",
                    nmeaBytes);
    } else {
      Serial.println("GPS WARNING — 0 bytes received. Check D7(RX)↔GPS-TX and D6(TX)↔GPS-RX wiring.");
    }
  }

  // ── I2C + BNO055 (7Semi breakout, tries 0x28 then 0x29) ───────────────────
  Wire.begin(SDA_PIN, SCL_PIN);  // SDA=GPIO5(D4), SCL=GPIO6(D5)
  Wire.setTimeOut(50);
  Serial.printf("I2C started: SDA=GPIO%d (D4), SCL=GPIO%d (D5)\n",
                SDA_PIN, SCL_PIN);

  sensorOK = startBNO055();
  Serial.println(sensorOK ? "BNO055 ready." : "BNO055 not found — will retry every 5 s.");

  // Start inactivity timer (assume moving on wake)
  inactiveStart      = millis();
  inactivityTracking = false;

  // Create the upload queue (depth=1; latest snapshot always wins)
  uploads = xQueueCreate(1, sizeof(Snapshot));

  if (!uploads ||
      xTaskCreatePinnedToCore(
        networkTask,
        "uploader",
        16384,     // 16 KB stack (more room for TLS + JSON)
        nullptr,
        1,
        nullptr,
        0          // pin to core 0 so loop() runs on core 1 unimpeded
      ) != pdPASS) {
    Serial.println("FATAL: uploader task could not start.");
    uploads = nullptr;
  }

  Serial.println("=== Boot complete ===");
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
  uint32_t now = millis();

  // ---- IMU retry if sensor failed ----
  static uint32_t retryAt  = 0;
  if (!sensorOK && uint32_t(now - retryAt) >= 5000) {
    retryAt  = now;
    sensorOK = startBNO055();
    if (sensorOK) Serial.println("BNO055 recovered.");
  }

  // ---- IMU polling ----
  static uint32_t sensorAt = 0;
  if (sensorOK && uint32_t(now - sensorAt) >= SENSOR_MS) {
    sensorAt = now;
    sensorOK = readBNO055();
    if (sensorOK) {
      float acceleration = sqrtf(
        accelEvent.acceleration.x * accelEvent.acceleration.x +
        accelEvent.acceleration.y * accelEvent.acceleration.y +
        accelEvent.acceleration.z * accelEvent.acceleration.z);
      float rotation = sqrtf(gyroEvent.gyro.x * gyroEvent.gyro.x +
                             gyroEvent.gyro.y * gyroEvent.gyro.y +
                             gyroEvent.gyro.z * gyroEvent.gyro.z);
      motionFilter.update(acceleration, rotation, now);
      if (!motionFilter.quietFor(now, 0)) stationaryAnchorValid = false;
    }
  }
  if (!sensorOK) {
    motionFilter.reset();
    stationaryAnchorValid = false;
  }

  // ---- Activity classification ----
  const char *act = classifyActivity();
  strlcpy(currentActivity, act, sizeof(currentActivity));

  // ---- Feed GPS NMEA bytes ----
  while (GPS_Serial.available()) {
    if (gps.encode(GPS_Serial.read())) {
      consumeFix();
    }
  }

  // ---- Inactivity / deep-sleep tracking ----
  if (!motionFilter.stationary(now) || (acceptedSpeedValid && acceptedSpeedMps > 0.5)) {
    // Only confirmed IMU stillness can start the sleep timer, regardless of label.
    inactiveStart      = now;
    inactivityTracking = false;
  } else {
    if (!inactivityTracking) {
      inactiveStart      = now;
      inactivityTracking = true;
    }
    if (!DEMO_MODE && uint32_t(now - inactiveStart) >= INACTIVITY_MS) {
      Serial.println("4-min inactivity threshold reached — entering deep sleep (10 min).");

      // Push one final status update before sleeping
      static uint32_t uploadAt = 0; // forward declaration handled below; reuse path
      // Build and enqueue final snapshot inline
      Snapshot final{};
      final.fixOK = haveFix &&
                    uint32_t(now - acceptedAt) < FRESH_MS &&
                    strcmp(assessment, "weak_fix")      != 0 &&
                    strcmp(assessment, "jump_rejected") != 0;

      const char *why =
        !haveFix                             ? assessment :
        uint32_t(now - acceptedAt) >= FRESH_MS ? "stale"   :
        assessment;
      strlcpy(final.reason,   why,            sizeof(final.reason));
      strlcpy(final.activity, currentActivity, sizeof(final.activity));
      final.lat       = acceptedLat;
      final.lon       = acceptedLon;
      final.epoch     = acceptedEpoch;
      final.satellites = gps.satellites.isValid() && gps.satellites.age() < FRESH_MS
                         ? (int)gps.satellites.value() : -1;
      final.hdop      = gps.hdop.isValid() && gps.hdop.age() < FRESH_MS
                        ? gps.hdop.hdop() : NAN;
      final.speed     = gps.speed.isValid() && gps.speed.age() < FRESH_MS
                        ? gps.speed.kmph() : NAN;
      final.altitude  = gps.altitude.isValid() && gps.altitude.age() < FRESH_MS
                        ? gps.altitude.meters() : NAN;
      final.sensorOK  = sensorOK;
      final.ax        = accelEvent.acceleration.x;
      final.ay        = accelEvent.acceleration.y;
      final.az        = accelEvent.acceleration.z;
      final.gx        = gyroEvent.gyro.x;
      final.gy        = gyroEvent.gyro.y;
      final.gz        = gyroEvent.gyro.z;
      final.apCount   = collectTopAPs(final.aps);

      if (uploads) xQueueOverwrite(uploads, &final);

      // Allow uploader task time to send before sleeping
      vTaskDelay(pdMS_TO_TICKS(3000));

      esp_sleep_enable_timer_wakeup(SLEEP_US);
      esp_deep_sleep_start(); // does not return
    }
  }

  // ---- Async Wi-Fi scan ----
  if (!scanStarted || uint32_t(now - lastScanAt) >= SCAN_INTERVAL_MS) {
    // Start a new async (non-blocking) scan; show_hidden=false, passive=false
    WiFi.scanNetworks(/*async=*/true);
    scanStarted = true;
    lastScanAt  = now;
  }

  // ---- Upload snapshot ----
  static uint32_t uploadAt = 0;
  if (uint32_t(now - uploadAt) >= UPLOAD_MS) {
    uploadAt = now;

    Snapshot s{};

    s.fixOK =
      haveFix &&
      uint32_t(now - acceptedAt) < FRESH_MS &&
      strcmp(assessment, "weak_fix")      != 0 &&
      strcmp(assessment, "jump_rejected") != 0;

    const char *why =
      !haveFix                               ? assessment :
      uint32_t(now - acceptedAt) >= FRESH_MS ? "stale"   :
      assessment;

    strlcpy(s.reason,   why,            sizeof(s.reason));
    strlcpy(s.activity, currentActivity, sizeof(s.activity));

    s.lat       = acceptedLat;
    s.lon       = acceptedLon;
    s.epoch     = acceptedEpoch;

    s.satellites =
      gps.satellites.isValid() && gps.satellites.age() < FRESH_MS
      ? (int)gps.satellites.value() : -1;

    s.hdop =
      gps.hdop.isValid() && gps.hdop.age() < FRESH_MS
      ? gps.hdop.hdop() : NAN;

    s.speed =
      gps.speed.isValid() && gps.speed.age() < FRESH_MS
      ? gps.speed.kmph() : NAN;

    s.altitude =
      gps.altitude.isValid() && gps.altitude.age() < FRESH_MS
      ? gps.altitude.meters() : NAN;

    s.sensorOK = sensorOK;
    s.ax = accelEvent.acceleration.x;
    s.ay = accelEvent.acceleration.y;
    s.az = accelEvent.acceleration.z;
    s.gx = gyroEvent.gyro.x;
    s.gy = gyroEvent.gyro.y;
    s.gz = gyroEvent.gyro.z;

    // Collect top-3 Wi-Fi APs from last completed scan
    s.apCount = collectTopAPs(s.aps);

    if (uploads) xQueueOverwrite(uploads, &s);

    // ---- Serial diagnostics ----
    Serial.printf(
      "[%lu] GPS:%s sats:%d hdop:%.2f | IMU:%s | activity:%s\n",
      now,
      s.reason,
      s.satellites,
      isfinite(s.hdop) ? s.hdop : -1.0,
      s.sensorOK ? "OK" : "FAIL",
      s.activity
    );

    if (s.fixOK) {
      Serial.printf("  → %.7f, %.7f | epoch:%.0f | spd:%.1f km/h | alt:%.1f m\n",
                    s.lat, s.lon, s.epoch, s.speed, s.altitude);
    }

    if (s.apCount > 0) {
      for (int i = 0; i < s.apCount; i++) {
        Serial.printf("  AP[%d] %s  RSSI:%d\n", i, s.aps[i].bssid, s.aps[i].rssi);
      }
    }
  }

  delay(1); // yield to FreeRTOS scheduler
}
