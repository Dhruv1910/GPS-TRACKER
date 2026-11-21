#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_BNO055.h>
#include <Adafruit_Sensor.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "secrets.h"

// XIAO ESP32-S3 original
// GPS: TinyGPSPlus
// IMU: Adafruit BNO055

TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);
Adafruit_BNO055 bno(55, 0x28, &Wire);

constexpr int GPS_RX = D7;
constexpr int GPS_TX = D6;
constexpr int SDA_PIN = D4;
constexpr int SCL_PIN = D5;

constexpr uint32_t FRESH_MS = 3000;
constexpr uint32_t UPLOAD_MS = 2000;
constexpr uint32_t SENSOR_MS = 50;
constexpr double MAX_HDOP = 5.0;
constexpr uint32_t MIN_SATELLITES = 4;
constexpr double MAX_SPEED_MPS = 60.0;
constexpr double JUMP_SLACK_M = 30.0;
constexpr uint32_t REACQUIRE_MS = 15000;

struct Snapshot {
  bool fixOK;
  bool sensorOK;
  double lat, lon, epoch, hdop, speed, altitude;
  int satellites;
  float ax, ay, az, gx, gy, gz;
  char reason[24];
};

QueueHandle_t uploads = nullptr;

bool sensorOK = false;
bool haveFix = false;

double acceptedLat = 0;
double acceptedLon = 0;
double acceptedEpoch = 0;

uint32_t acceptedAt = 0;
const char *assessment = "waiting";

sensors_event_t accelEvent;
sensors_event_t gyroEvent;

// Convert GPS date/time into Unix epoch seconds.
double gpsEpoch() {
  int y = gps.date.year();
  int m = gps.date.month();
  int d = gps.date.day();

  if (y < 2020 || y > 2099 ||
      m < 1 || m > 12 || d < 1 || d > 31) {
    return 0;
  }

  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

  int64_t days = (int64_t)era * 146097 + doe - 719468;

  return double(days * 86400LL +
    gps.time.hour() * 3600LL +
    gps.time.minute() * 60LL +
    gps.time.second()) +
    gps.time.centisecond() / 100.0;
}

void consumeFix() {
  if (!gps.location.isUpdated()) return;

  uint32_t age = gps.location.age();
  double lat = gps.location.lat();
  double lon = gps.location.lng();

  if (!gps.location.isValid() ||
      age > FRESH_MS ||
      !gps.date.isValid() ||
      !gps.time.isValid() ||
      gps.date.age() > FRESH_MS ||
      gps.time.age() > FRESH_MS) {
    return;
  }

  double hdop = gps.hdop.isValid() ? gps.hdop.hdop() : NAN;
  int sats = gps.satellites.isValid()
    ? (int)gps.satellites.value() : 0;

  Serial.printf("RAW,%.2f,%.8f,%.8f,%.2f,%d\n",
                gpsEpoch(), lat, lon, hdop, sats);

  bool quality =
    gps.hdop.isValid() &&
    gps.hdop.age() < FRESH_MS &&
    gps.satellites.isValid() &&
    gps.satellites.age() < FRESH_MS &&
    hdop > 0 &&
    hdop <= MAX_HDOP &&
    sats >= MIN_SATELLITES;

  if (!quality) {
    assessment = "weak_fix";
    return;
  }

  double epoch = gpsEpoch();

  if (epoch == 0 ||
      (haveFix && epoch <= acceptedEpoch)) {
    return;
  }

  uint32_t now = millis();

  bool reacquire = haveFix &&
    uint32_t(now - acceptedAt) >= REACQUIRE_MS;

  if (haveFix && !reacquire) {
    double elapsed = epoch - acceptedEpoch;

    double distance = TinyGPSPlus::distanceBetween(
      acceptedLat, acceptedLon, lat, lon
    );

    if (distance > JUMP_SLACK_M + MAX_SPEED_MPS * elapsed) {
      assessment = "jump_rejected";
      return;
    }
  }

  acceptedLat = lat;
  acceptedLon = lon;
  acceptedEpoch = epoch;
  acceptedAt = now - age;
  haveFix = true;

  assessment = reacquire ? "reacquired" : "fresh";
}

bool startBNO055() {
  if (!bno.begin()) {
    return false;
  }

  delay(1000);
  bno.setExtCrystalUse(true);

  return true;
}

bool readBNO055() {
  if (!sensorOK) return false;

  bool ok1 = bno.getEvent(
    &accelEvent,
    Adafruit_BNO055::VECTOR_ACCELEROMETER
  );

  bool ok2 = bno.getEvent(
    &gyroEvent,
    Adafruit_BNO055::VECTOR_GYROSCOPE
  );

  if (!ok1 || !ok2) return false;

  return
    isfinite(accelEvent.acceleration.x) &&
    isfinite(accelEvent.acceleration.y) &&
    isfinite(accelEvent.acceleration.z) &&
    isfinite(gyroEvent.gyro.x) &&
    isfinite(gyroEvent.gyro.y) &&
    isfinite(gyroEvent.gyro.z);
}

void networkTask(void *parameter) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  configTime(0, 0, "pool.ntp.org", "time.google.com");

  uint32_t lastReconnect = millis();
  uint32_t backoffMS = 2000;

  for (;;) {
    Snapshot s;

    if (xQueueReceive(
          uploads, &s, pdMS_TO_TICKS(1000)) != pdTRUE) {
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (uint32_t(millis() - lastReconnect) >= 10000) {
        lastReconnect = millis();
        WiFi.reconnect();
      }
      continue;
    }

    String url(API_URL);
    bool secure = url.startsWith("https://");

    if (!secure &&
        !(ALLOW_LAN_HTTP && url.startsWith("http://"))) {
      Serial.println("Blocked: HTTPS required.");
      continue;
    }

    if (secure &&
        (time(nullptr) < 1704067200 ||
         !strstr(ROOT_CA, "BEGIN CERTIFICATE"))) {
      Serial.println("HTTPS waiting: check NTP and ROOT_CA.");
      continue;
    }

    if (s.fixOK &&
        time(nullptr) >= 1704067200 &&
        difftime(time(nullptr), (time_t)s.epoch) > 10) {
      s.fixOK = false;
      strlcpy(s.reason, "stale", sizeof(s.reason));
    }

    JsonDocument doc;

    doc["fix_ok"] = s.fixOK;
    doc["mpu_ok"] = s.sensorOK;
    doc["reason"] = s.reason;

    if (s.fixOK) {
      doc["lat"] = s.lat;
      doc["lon"] = s.lon;
      doc["fix_epoch"] = s.epoch;
    }

    if (s.satellites >= 0)
      doc["satellites"] = s.satellites;

    if (isfinite(s.hdop))
      doc["hdop"] = s.hdop;

    if (isfinite(s.speed))
      doc["speed_kmph"] = s.speed;

    if (isfinite(s.altitude))
      doc["altitude_m"] = s.altitude;

    if (s.sensorOK) {
      doc["ax"] = s.ax;
      doc["ay"] = s.ay;
      doc["az"] = s.az;
      doc["gx"] = s.gx;
      doc["gy"] = s.gy;
      doc["gz"] = s.gz;
    }

    String payload;
    serializeJson(doc, payload);

    WiFiClient plain;
    WiFiClientSecure tls;
    HTTPClient http;

    tls.setCACert(ROOT_CA);
    tls.setHandshakeTimeout(8);

    http.setConnectTimeout(8000);
    http.setTimeout(8000);

    bool begun = secure
      ? http.begin(tls, url)
      : http.begin(plain, url);

    int status = -1;

    if (begun) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader(
        "Authorization",
        String("Bearer ") + DEVICE_TOKEN
      );

      status = http.POST(payload);

      String response = http.getString();
      Serial.printf("Upload HTTP status: %d\n", status);

      if (status < 200 || status >= 300) {
        Serial.println(response);
      }

      http.end();
    }

    if (status >= 200 && status < 300) {
      backoffMS = 2000;
    } else {
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

  sensorOK = startBNO055();

  Serial.println(
    sensorOK ? "BNO055 ready" : "BNO055 unavailable"
  );

  uploads = xQueueCreate(1, sizeof(Snapshot));

  if (!uploads ||
      xTaskCreate(
        networkTask,
        "uploader",
        12288,
        nullptr,
        1,
        nullptr
      ) != pdPASS) {
    Serial.println("Uploader could not start.");
    uploads = nullptr;
  }
}

void loop() {
  while (GPS_Serial.available()) {
    if (gps.encode(GPS_Serial.read())) {
      consumeFix();
    }
  }

  uint32_t now = millis();

  static uint32_t sensorAt = 0;
  static uint32_t retryAt = 0;
  static uint32_t uploadAt = 0;

  if (!sensorOK &&
      uint32_t(now - retryAt) >= 5000) {
    retryAt = now;
    sensorOK = startBNO055();
  }

  if (sensorOK &&
      uint32_t(now - sensorAt) >= SENSOR_MS) {
    sensorAt = now;
    sensorOK = readBNO055();
  }

  if (uint32_t(now - uploadAt) >= UPLOAD_MS) {
    uploadAt = now;

    Snapshot s{};

    s.fixOK =
      haveFix &&
      uint32_t(now - acceptedAt) < FRESH_MS &&
      strcmp(assessment, "weak_fix") != 0 &&
      strcmp(assessment, "jump_rejected") != 0;

    const char *why =
      !haveFix ? assessment :
      uint32_t(now - acceptedAt) >= FRESH_MS ? "stale" :
      assessment;

    strlcpy(s.reason, why, sizeof(s.reason));

    s.lat = acceptedLat;
    s.lon = acceptedLon;
    s.epoch = acceptedEpoch;

    s.satellites =
      gps.satellites.isValid() &&
      gps.satellites.age() < FRESH_MS
      ? (int)gps.satellites.value() : -1;

    s.hdop =
      gps.hdop.isValid() &&
      gps.hdop.age() < FRESH_MS
      ? gps.hdop.hdop() : NAN;

    s.speed =
      gps.speed.isValid() &&
      gps.speed.age() < FRESH_MS
      ? gps.speed.kmph() : NAN;

    s.altitude =
      gps.altitude.isValid() &&
      gps.altitude.age() < FRESH_MS
      ? gps.altitude.meters() : NAN;

    s.sensorOK = sensorOK;

    s.ax = accelEvent.acceleration.x;
    s.ay = accelEvent.acceleration.y;
    s.az = accelEvent.acceleration.z;

    s.gx = gyroEvent.gyro.x;
    s.gy = gyroEvent.gyro.y;
    s.gz = gyroEvent.gyro.z;

    if (uploads) {
      xQueueOverwrite(uploads, &s);
    }

    Serial.printf(
      "GPS: %s | Satellites: %d | HDOP: %.2f | BNO055: %s\n",
      s.reason,
      s.satellites,
      s.hdop,
      s.sensorOK ? "OK" : "NOT READY"
    );

    if (s.fixOK) {
      Serial.printf(
        "Location: %.6f, %.6f | UTC epoch: %.2f\n",
        s.lat, s.lon, s.epoch
      );
    }
  }

  delay(1);
}
