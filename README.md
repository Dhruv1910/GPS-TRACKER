# TMI-103 — IoT Animal Tracking Collar

> **Hardware:** ESP32 · TinyGPS++ (UART GPS) · Adafruit BNO055 9-axis IMU  
> **Stack:** Arduino C++ firmware · Flask 3 backend · Leaflet.js dashboard · Capacitor Android APK

---

## Project structure

```
GPS-TRACKER/
├── app.py                   # Flask API and notifications
├── geofence_filter.py       # GPS confirmation and boundary hysteresis
├── tracker/                 # ESP32 firmware, motion filter, private secrets
├── static/                  # Dashboard and saved-login logic
├── android/                 # Native Android project and encrypted storage plugin
├── scripts/                 # Web build and APK export scripts
├── tests/                   # Geofence, tracker name, login, and motion tests
├── docs/references/         # Hardware list and IITR IoT setup guide
├── releases/                # Latest installable field-tracker.apk (not committed)
├── capacitor.config.json
├── package.json
├── package-lock.json
├── requirements.txt
└── README.md
```

Private configuration remains in its required locations: `config.local.json`,
`firebase-admin.json`, `geofence.local.json`, `tracker.local.json`,
`tracker/secrets.h`, and `android/app/google-services.json`. These are ignored by
Git. Keep them when moving or backing up your local project.

`node_modules/` contains the installed build tools, including the local Node 22
runtime. Generated `www/`, Python caches, Gradle caches and `build/` directories
can be recreated. Run `npm run cap:sync` before opening/building Android after a
cleanup. After building, `npm run apk:export` copies the APK to
`releases/field-tracker.apk`; `npm run apk:debug` builds and exports it together.


---

## 1 · Flash the ESP32 firmware

### Hardware: Seeed Studio XIAO ESP32-S3

#### Breadboard pin-to-wire mapping

| XIAO Label | GPIO  | Wire colour (suggestion) | Connected To         |
|-----------|-------|--------------------------|----------------------|
| **D4**    | 5     | Yellow                   | BNO055 **SDA**       |
| **D5**    | 6     | Green                    | BNO055 **SCL**       |
| **D6**    | 43    | Blue                     | NEO-M8N **RX** wire  |
| **D7**    | 44    | White                    | NEO-M8N **TX** wire  |
| **3V3**   | —     | Red                      | BNO055 VIN, GPS VCC  |
| **GND**   | —     | Black                    | BNO055 GND, GPS GND  |

> **Note TX/RX cross:** XIAO D7 (GPIO 44) is the **firmware RX** pin — connect it to the **GPS module's TX** pad. XIAO D6 (GPIO 43) is the **firmware TX** pin — connect it to the **GPS module's RX** pad.

#### Arduino IDE board settings

1. **Tools → Board → esp32 → "XIAO_ESP32S3"**
2. **Tools → USB CDC On Boot → "Enabled"** ← *required for Serial Monitor output over USB*
3. **Tools → Port → select the XIAO's USB serial port**

### Steps

```bash
# 1. Copy secrets template
cp tracker/secrets.h.template tracker/secrets.h

# 2. Edit tracker/secrets.h — fill in Wi-Fi passwords, server URL, device token

# 3. Open tracker/tracker.ino in Arduino IDE 2.x
#    Board:            esp32 → XIAO_ESP32S3
#    USB CDC On Boot:  Enabled   ← required for Serial Monitor over USB
#    Required libraries (install via Library Manager):
#      - TinyGPSPlus        (search "TinyGPS++" by Mikal Hart)
#      - Adafruit BNO055    (click "Install All" when prompted for dependencies)
#      - Adafruit Unified Sensor
#      - ArduinoJson  v7+

# 4. Click Upload, then open Serial Monitor at 115200 baud
#    First lines printed:
#      Device MAC: XX:XX:XX:XX:XX:XX   ← register this with IITR ICC
#      GPS wiring check: ...           ← confirms NEO-M8N TX/RX wiring
#      BNO055 found at I2C address 0x28 (or 0x29 on some 7Semi boards)
```

---

## 2 · Deploy the Flask backend

### Configured local setup

Your Mac's server address is `http://10.165.39.84:8000`.
`config.local.json` contains the existing firmware device token and your supplied
view token. This private file and `tracker/secrets.h` are ignored by Git.
Environment variables override the local settings; unset old placeholder values
before starting:

```bash
unset DEVICE_TOKEN VIEW_TOKEN
python3 app.py
```

Keep that terminal running. Open `http://localhost:8000` on this Mac, or
`http://10.165.39.84:8000` from a device on the same network, then enter your
view token in the dashboard. The token is stored only in the private server
configuration; enter it manually in the dashboard.

Re-upload `tracker/tracker.ino` to apply the corrected firmware endpoint
`http://10.165.39.84:8000/api/telemetry`. If your Mac's IP changes, update
`tracker/secrets.h` and the LAN address in `static/map.js`.


### Environment variables required

| Variable                  | Description                                    |
|---------------------------|------------------------------------------------|
| `DEVICE_TOKEN`            | ≥32-char secret — must match firmware `secrets.h` |
| `VIEW_TOKEN`              | ≥32-char secret — used by the browser dashboard |
| `HOME_LAT` / `HOME_LNG`  | Geofence centre (decimal degrees)              |
| `GEOFENCE_RADIUS_METERS`  | Geofence radius in metres (default `50.0`)     |

```bash
# Install dependencies
pip install -r requirements.txt

# Export secrets (Linux/macOS)
export DEVICE_TOKEN="your_very_long_device_token_here_32chars"
export VIEW_TOKEN="your_very_long_view_token_here_32chars_"
export HOME_LAT=29.8656
export HOME_LNG=77.8961
export GEOFENCE_RADIUS_METERS=50

# Run locally (dev)
python app.py

# Run in production (gunicorn, single worker — RAM state)
gunicorn -w 1 -b 0.0.0.0:8000 app:app
```

> **Note:** The server uses in-RAM state with a single Gunicorn worker (`-w 1`).  
> For multi-worker deployments, replace `deque` / `state` dict with Redis.

---

## 3 · Add Firebase credentials (optional — enables push notifications)

```bash
# a) In Firebase Console → Project Settings → Service Accounts
#    → Generate new private key → save as firebase-admin.json
cp ~/Downloads/firebase-admin.json ./firebase-admin.json

# b) In your Android app → Project Settings → google-services.json
#    (placed inside the android/ folder after Capacitor setup)
```

---

## 4 · Known Wi-Fi APs for indoor localisation

Edit `KNOWN_WIFI_APS` in `app.py` with your campus router BSSIDs:

```python
KNOWN_WIFI_APS = {
    "aa:bb:cc:dd:ee:01": (29.8656, 77.8961),   # Main building AP
    "aa:bb:cc:dd:ee:02": (29.8660, 77.8970),   # Library AP
}
```

You can discover BSSIDs on-site with:
```bash
# macOS
/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport -s

# Linux
sudo iw dev wlan0 scan | grep BSSID
```

---

## 5 · Build the Android APK (Capacitor)

The Android project has been generated in `android/`. The web build bundles the
notification plugins into `www/`; `npm run cap:sync` copies those assets to Android.
The project includes a local Node 22 runtime so npm scripts work on this Mac.

### Install the missing Android tools

Install Android Studio from https://developer.android.com/studio and complete its
setup wizard. Install Android SDK Platform 34 and Build Tools 34.0.0 in SDK Manager.
Use JDK 17 for Gradle 8.2.1. The Mac's Java 11 is too old and the installed
Android Studio's bundled Java 25 is too new for this project's Gradle version. Open this repository's `android/` directory in Android
Studio and let Gradle sync finish.

### Enable background notifications

The signed-in Firebase account had no projects when checked. Create a project at
https://console.firebase.google.com/ and register an Android app with package
`com.iitr.fieldtracker`.

1. Download its `google-services.json` to `android/app/google-services.json`.
2. Under Project settings → Service accounts, generate a private key and save it
   as `firebase-admin.json` beside `app.py`. Keep it private; never put it in the APK.
3. Restart Flask to load the server credentials.
4. Rebuild/sync the app. Push registration is enabled in the native bundle when
   `android/app/google-services.json` exists. Allow notifications on the phone.

```bash
npm run cap:sync
npm run cap:open
```

In Android Studio, build the debug APK. The output is
`android/app/build/outputs/apk/debug/app-debug.apk`. Run `npm run apk:export`
to copy it to `releases/field-tracker.apk`.
Transfer it by USB and install it on the Android phone. Enter the existing view
token in the app. Connect the phone and Mac to the same network for this local
prototype. Flask must remain running and the Mac must remain awake.

The app uses native HTTP requests for the local server, with cleartext permission
limited to `10.165.39.84`. Update the address in `static/map.js`,
`tracker/secrets.h`, and `android/app/src/main/res/xml/network_security_config.xml`
if the Mac's IP changes. Deploy Flask to HTTPS for use away from this LAN.

### End-to-end checks still required

- Verify live telemetry on the phone.
- Set `HOME_LAT`, `HOME_LNG`, and `GEOFENCE_RADIUS_METERS` to the actual test area.
- Walk outside that boundary, then verify a notification with the app in the
  background and the screen locked. Android force-stop can prevent delivery.
- Replace example Wi-Fi AP entries with real surveyed BSSIDs and coordinates.
- Check activity classification at rest and in motion, and verify sleep/wake timing.

---

## 6 · Dashboard quick-start

1. Open `http://localhost:8000` on this Mac, or the server's LAN URL on your phone.
2. Enter the existing viewer token once. The app reconnects with the saved token
   after closing/reopening. **Disconnect** removes it. Android stores the token
   encrypted with an Android Keystore key in a file excluded from backup; the
   browser uses local storage scoped to that server address. Clearing app/browser
   data also clears the login. Rejected tokens are removed; network outages do not
   erase a saved login.
3. Under **Set safe zone**, enter centre latitude/longitude, choose a point on the
   map, or use the tracker's fresh location. Set the radius in metres.
4. Review the dashed blue preview, then press **Save safe zone**. **Reset edits**
   discards changes that have not been saved. Polling preserves your draft.

The backend validates coordinates and a radius between 1 and 100,000 metres,
authorizes changes with the viewer token, and saves the boundary privately in
`geofence.local.json`. The saved boundary applies to all viewers of this single
tracker and survives server restarts. Environment variables supply initial defaults
only when no saved boundary exists. No firmware upload is needed to change it.
After saving, the next fresh telemetry update evaluates alerts against the new
boundary. Missing/stale positions show an unknown geofence state, rather than
claiming the animal is inside the zone.

To get these controls on a previously installed phone app, install the rebuilt
`releases/field-tracker.apk` as an update. Browser users can
refresh the dashboard. Test backend geofence behavior with:

```bash
python3 -m unittest discover -s tests -v
```

---

## 7 · Deep-sleep behaviour

| State      | Upload interval | Sleep after |
|------------|----------------|-------------|
| Moving     | every 2 s      | never       |
| Resting / Grazing | every 2 s | 4 min inactivity → sleep |
| Sleeping   | —              | wakes every 10 min |

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `401 Unauthorized` from firmware | `DEVICE_TOKEN` in `secrets.h` ≠ server env var |
| `GPS: waiting` after 5 min | Move to open sky; check UART wiring RX↔TX swap |
| BNO055 NOT READY | Check I²C address (0x28 vs 0x29), confirm SDA/SCL wiring |
| Map tiles missing | No internet on the viewing device; coords still work |
| FCM alerts not arriving | Confirm `firebase-admin.json` placed next to `app.py`; check `google-services.json` in Android project |

## GPS drift protection and confirmed geofence alerts

- Firmware reads gravity-free BNO055 linear acceleration plus gyroscope readings.
  Quiet motion means acceleration below 0.3 m/s² and rotation below 0.08 rad/s.
  Two seconds of continuous quiet motion establishes `Resting`.
- Drift protection does not depend on the activity label. After 0.5 seconds of
  quiet IMU readings, a previously accepted speed at or below 0.5 m/s protects the
  last position even before the activity changes to Resting. Two seconds of IMU
  stillness can also establish protection if GPS speed is unavailable. A known
  accepted speed above 0.5 m/s prevents a stationary lock even with a quiet IMU.
- While protected, jumps exceeding 8 m from a stationary anchor are rejected.
  Smaller offsets do not move the anchor, preventing gradual accumulated drift.
  This protection remains during GPS reacquisition; detected movement or invalid
  IMU data releases the anchor. IMU stillness alone cannot prove zero velocity;
  verify thresholds with real stationary and moving tests.
- Backend alerts require three distinct, increasing, fresh GPS timestamps outside
  the radius. Duplicate/reordered timestamps do not advance the count. Invalid,
  stale or missing GPS breaks the streak, as does a gap longer than 10 seconds.
  Wi-Fi-only positions remain visible but do not confirm geofence alerts.
- After an alert, returning just inside the boundary does not re-arm it. The
  tracker must move inside by 10 m, or 20% of the radius for zones smaller than
  50 m. For example, a 50 m zone re-arms at 40 m from the centre. A new breach
  then needs three new outside fixes. Saving a new zone clears confirmation state.
- The dashboard shows pending GPS confirmation and the re-entry buffer state.
  Install the rebuilt APK to see these states. Re-upload `tracker/tracker.ino`
  to activate the firmware changes; the backend does not upgrade the board.

Validation:

```bash
python3 -m unittest discover -s tests -v
c++ -std=c++11 tests/motion_filter_test.cpp -o /tmp/gps-motion-filter-test
/tmp/gps-motion-filter-test
node_modules/node/bin/node --test tests/token-session.test.cjs
```

## Tracker name

The default name is **animal-01**. After connecting, edit **Tracker name** and
press **Save name**. The name is shared by this tracker across connected viewers,
is saved in the private `tracker.local.json` file, and survives server restarts.
Names may contain 1–64 characters; surrounding spaces are trimmed and control
characters are rejected. Names are rendered as text, not HTML.

Geofence notifications include the saved name in both the title and body, e.g.
`Daisy — geofence alert` and `Daisy is 75 m from the safe-zone centre.` Renaming
does not reset geofence confirmation or re-arm an active alert. Re-uploading the
firmware is not required for a name change. Install the updated APK to edit it
from the Android app, or refresh the web dashboard.
