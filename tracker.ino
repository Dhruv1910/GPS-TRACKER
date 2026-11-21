// XIAO ESP32-S3 (original, not Plus). Arduino-ESP32 3.x, ArduinoJson 7.x.
// Libraries: TinyGPSPlus, Adafruit MPU6050, Adafruit Unified Sensor,
// Adafruit BusIO, ArduinoJson. TinyGPSPlus's header is TinyGPS++.h.
#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "secrets.h"

TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);
Adafruit_MPU6050 mpu;
constexpr int GPS_RX = D7, GPS_TX = D6, SDA_PIN = D4, SCL_PIN = D5;
constexpr uint32_t FRESH_MS = 3000, UPLOAD_MS = 2000, SENSOR_MS = 50;
constexpr double MAX_HDOP = 5.0;
constexpr uint32_t MIN_SATELLITES = 4;
// Broad gross-jump guard, not a claim about accuracy. 60 m/s = 216 km/h.
// For a dedicated walking test try 15 m/s, then validate false rejections.
constexpr double MAX_SPEED_MPS = 60.0, JUMP_SLACK_M = 30.0;
constexpr uint32_t REACQUIRE_MS = 15000;

struct Snapshot {
  bool fixOK, mpuOK;
  double lat, lon, epoch, hdop, speed, altitude;
  int satellites;
  float ax, ay, az, gx, gy, gz;
  char reason[24];
};
QueueHandle_t uploads;
bool mpuOK = false, haveFix = false;
double acceptedLat = 0, acceptedLon = 0, acceptedEpoch = 0;
uint32_t acceptedAt = 0;
const char *assessment = "waiting";
sensors_event_t accel{}, gyro{}, temp{};

// Convert UTC GPS calendar fields to Unix seconds, independent of system time.
// Gregorian civil-date calculation (days since 1970-01-01).
double gpsEpoch() {
  int y = gps.date.year(), m = gps.date.month(), d = gps.date.day();
  if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  y -= m <= 2;
  int era = y / 400, yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + doe - 719468;
  return double(days * 86400LL + gps.time.hour() * 3600LL + gps.time.minute() * 60LL + gps.time.second()) + gps.time.centisecond() / 100.0;
}

void consumeFix() {
  if (!gps.location.isUpdated()) return;
  uint32_t age = gps.location.age();
  // Reading lat/lon clears isUpdated(), so test it BEFORE these calls.
  double lat = gps.location.lat(), lon = gps.location.lng();
  if (!gps.location.isValid() || age > FRESH_MS || !gps.date.isValid() ||
      !gps.time.isValid() || gps.date.age() > FRESH_MS || gps.time.age() > FRESH_MS) return;
  // Raw test log BEFORE quality/jump screening; GGA/RMC can duplicate an epoch.
  // Log columns: RAW,unix_seconds,latitude,longitude,HDOP,satellites.
  Serial.printf("RAW,%.2f,%.8f,%.8f,%.2f,%lu\n", gpsEpoch(), lat, lon,
    gps.hdop.isValid() ? gps.hdop.hdop() : NAN,
    gps.satellites.isValid() ? (unsigned long)gps.satellites.value() : 0UL);
  bool quality = gps.hdop.isValid() && gps.hdop.age() < FRESH_MS &&
    gps.satellites.isValid() && gps.satellites.age() < FRESH_MS &&
    gps.hdop.hdop() > 0 && gps.hdop.hdop() <= MAX_HDOP && gps.satellites.value() >= MIN_SATELLITES;
  if (!quality) {assessment = "weak_fix"; return;}
  double epoch = gpsEpoch();
  if (epoch == 0 || (haveFix && epoch <= acceptedEpoch)) return;
  uint32_t now = millis();
  bool reacquire = haveFix && uint32_t(now - acceptedAt) >= REACQUIRE_MS;
  if (haveFix && !reacquire) {
    double elapsed = epoch - acceptedEpoch;
    double distance = TinyGPSPlus::distanceBetween(acceptedLat, acceptedLon, lat, lon);
    if (distance > JUMP_SLACK_M + MAX_SPEED_MPS * elapsed) {
      assessment = "jump_rejected"; return;
    }
  }
  acceptedLat = lat; acceptedLon = lon; acceptedEpoch = epoch;
  acceptedAt = now - age; haveFix = true;
  assessment = reacquire ? "reacquired" : "fresh";
}

bool startMPU() {
  if (!mpu.begin(0x68, &Wire)) return false; // Change to 0x69 if AD0 is high.
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  return true;
}

void networkTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t lastReconnect = millis();
  uint32_t backoffMS = 2000;
  for (;;) {
    Snapshot s;
    if (xQueueReceive(uploads, &s, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
    if (WiFi.status() != WL_CONNECTED) {
      if (uint32_t(millis() - lastReconnect) >= 10000) {
        lastReconnect = millis(); WiFi.reconnect();
      }
      continue; // No backlog: next request will use the newest snapshot.
    }
    String url(API_URL);
    bool secure = url.startsWith("https://");
    if (!secure && !(ALLOW_LAN_HTTP && url.startsWith("http://"))) {
      Serial.println("Blocked: use HTTPS, or explicitly enable trusted LAN HTTP.");
      vTaskDelay(pdMS_TO_TICKS(5000)); continue;
    }
    if (secure && (time(nullptr) < 1704067200 || !strstr(ROOT_CA, "BEGIN CERTIFICATE"))) {
      Serial.println("HTTPS waiting: check NTP connectivity and ROOT_CA in secrets.h.");
      vTaskDelay(pdMS_TO_TICKS(5000)); continue;
    }
    // Discard a queued position that has become old while networking was slow.
    // Server independently checks GPS measurement timestamp against server UTC.
    if (s.fixOK && time(nullptr) >= 1704067200 && difftime(time(nullptr), (time_t)s.epoch) > 10) {
      s.fixOK = false; strlcpy(s.reason, "stale", sizeof(s.reason));
    }
    JsonDocument doc;
    doc["fix_ok"] = s.fixOK; doc["mpu_ok"] = s.mpuOK; doc["reason"] = s.reason;
    if (s.fixOK) {doc["lat"] = s.lat; doc["lon"] = s.lon; doc["fix_epoch"] = s.epoch;}
    if (s.satellites >= 0) doc["satellites"] = s.satellites;
    if (isfinite(s.hdop)) doc["hdop"] = s.hdop;
    if (isfinite(s.speed)) doc["speed_kmph"] = s.speed;
    if (isfinite(s.altitude)) doc["altitude_m"] = s.altitude;
    if (s.mpuOK) {
      doc["ax"] = s.ax; doc["ay"] = s.ay; doc["az"] = s.az;
      doc["gx"] = s.gx; doc["gy"] = s.gy; doc["gz"] = s.gz;
    }
    String payload; serializeJson(doc, payload);
    WiFiClient plain;
    WiFiClientSecure tls;
    HTTPClient http;
    tls.setCACert(ROOT_CA);
    tls.setHandshakeTimeout(8);
    http.setConnectTimeout(8000); http.setTimeout(8000);
    bool begun = secure ? http.begin(tls, url) : http.begin(plain, url);
    int status = -1;
    if (begun) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
      status = http.POST(payload);
      http.end();
    }
    Serial.printf("Upload HTTP status: %d\n", status); // Never print credentials.
    if (status >= 200 && status < 300) backoffMS = 2000;
    else {
      vTaskDelay(pdMS_TO_TICKS(backoffMS));
      backoffMS = min(backoffMS * 2U, 30000U);
    }
  }
}

void setup() {
  Serial.begin(115200);
  GPS_Serial.setRxBufferSize(2048);
  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(50);
  mpuOK = startMPU();
  Serial.println(mpuOK ? "MPU6050 ready" : "MPU6050 unavailable; GPS continues");
  uploads = xQueueCreate(1, sizeof(Snapshot));
  if (!uploads || xTaskCreate(networkTask, "uploader", 12288, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("Uploader could not start; restart device.");
    uploads = nullptr; // Serial GPS output can still operate.
  }
}

void loop() {
  while (GPS_Serial.available()) {
    if (gps.encode(GPS_Serial.read())) consumeFix();
  }
  uint32_t now = millis();
  static uint32_t sensorAt = 0, retryAt = 0, uploadAt = 0;
  if (!mpuOK && uint32_t(now - retryAt) >= 5000) {retryAt = now; mpuOK = startMPU();}
  if (mpuOK && uint32_t(now - sensorAt) >= SENSOR_MS) {
    sensorAt = now;
    Wire.beginTransmission(0x68); // Keep in sync with startMPU address.
    mpuOK = Wire.endTransmission() == 0;
    if (mpuOK) mpuOK = mpu.getEvent(&accel, &gyro, &temp);
    if (mpuOK) mpuOK = isfinite(accel.acceleration.x) && isfinite(accel.acceleration.y) &&
      isfinite(accel.acceleration.z) && isfinite(gyro.gyro.x) && isfinite(gyro.gyro.y) && isfinite(gyro.gyro.z);
  }
  if (uint32_t(now - uploadAt) >= UPLOAD_MS) {
    uploadAt = now;
    Snapshot s{};
    s.fixOK = haveFix && uint32_t(now - acceptedAt) < FRESH_MS &&
      strcmp(assessment, "weak_fix") != 0 && strcmp(assessment, "jump_rejected") != 0;
    const char *why = !haveFix ? assessment : uint32_t(now - acceptedAt) >= FRESH_MS ? "stale" : assessment;
    strlcpy(s.reason, why, sizeof(s.reason));
    s.lat = acceptedLat; s.lon = acceptedLon; s.epoch = acceptedEpoch;
    s.satellites = gps.satellites.isValid() && gps.satellites.age() < FRESH_MS ? (int)gps.satellites.value() : -1;
    s.hdop = gps.hdop.isValid() && gps.hdop.age() < FRESH_MS ? gps.hdop.hdop() : NAN;
    s.speed = gps.speed.isValid() && gps.speed.age() < FRESH_MS ? gps.speed.kmph() : NAN;
    s.altitude = gps.altitude.isValid() && gps.altitude.age() < FRESH_MS ? gps.altitude.meters() : NAN;
    s.mpuOK = mpuOK;
    s.ax = accel.acceleration.x; s.ay = accel.acceleration.y; s.az = accel.acceleration.z;
    s.gx = gyro.gyro.x; s.gy = gyro.gyro.y; s.gz = gyro.gyro.z;
    if (uploads) xQueueOverwrite(uploads, &s);
    Serial.printf("Fix: %s, satellites: %d, HDOP: %.2f, NMEA chars: %lu\n", s.reason, s.satellites, s.hdop, (unsigned long)gps.charsProcessed());
    if (s.fixOK) Serial.printf("%.6f, %.6f UTC epoch %.2f\n", s.lat, s.lon, s.epoch);
  }
  delay(1); // Yield CPU; network waits occur only in the uploader task.
}
