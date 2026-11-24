"""
GPS Animal Tracking Collar — Flask Backend
==========================================
Requires: Flask, flask-cors, firebase-admin (optional), gunicorn
Env vars:
  DEVICE_TOKEN  — Bearer token the ESP32 sends
  VIEW_TOKEN    — Bearer token the browser dashboard sends
  HOME_LAT / HOME_LNG — Geofence centre (default: IIT Roorkee main building)
  GEOFENCE_RADIUS_METERS — Geofence radius (default 50 m)
"""

import copy
import hmac
import json
from pathlib import Path
import math
import os
import threading
import time
from collections import deque

from flask import Flask, jsonify, request, send_from_directory, abort
from flask_cors import CORS
from geofence_filter import GeofenceFilter
from settings_store import SettingsStore, SettingsUnavailable

# ---------------------------------------------------------------------------
# Firebase / FCM — optional; gracefully disabled when creds are absent
# ---------------------------------------------------------------------------
try:
    import firebase_admin
    from firebase_admin import credentials, messaging as fcm_messaging

    _fb_cred_path = os.environ.get("FIREBASE_CREDENTIALS_PATH") or next(
        (str(path) for path in [Path(__file__).with_name("firebase-admin.json"),
                               Path("/etc/secrets/firebase-admin.json")] if path.exists()),
        str(Path(__file__).with_name("firebase-admin.json"))
    )
    if os.path.exists(_fb_cred_path):
        firebase_admin.initialize_app(
            credentials.Certificate(_fb_cred_path)
        )
        FCM_ENABLED = True
    else:
        FCM_ENABLED = False
        print(
            "⚠  firebase-admin.json not found — FCM push notifications disabled."
            " Place the file next to app.py to enable them."
        )
except ImportError:
    FCM_ENABLED = False
    print(
        "⚠  firebase-admin not installed — FCM push notifications disabled."
        " pip install firebase-admin to enable them."
    )

if os.environ.get("REQUIRE_FCM", "false").lower() == "true" and not FCM_ENABLED:
    raise RuntimeError("Firebase credentials are required for this deployment")

_settings_backend = os.environ.get("SETTINGS_BACKEND", "local")
_firestore_client = None
if _settings_backend == "firestore":
    if not FCM_ENABLED:
        raise RuntimeError("Firestore settings require Firebase credentials")
    from firebase_admin import firestore
    _firestore_client = firestore.client()
settings_store = SettingsStore(_settings_backend, _firestore_client,
                               os.environ.get("TRACKER_ID", "animal-01"))

# ---------------------------------------------------------------------------
# App setup
# ---------------------------------------------------------------------------
app = Flask(__name__, static_folder="static")
app.config["MAX_CONTENT_LENGTH"] = 8192

CORS(app, resources={r"/api/*": {"origins": "*"}})

# ---------------------------------------------------------------------------
# Auth tokens
# ---------------------------------------------------------------------------
# Local settings stay out of Git. Explicit environment variables take precedence.
_config_path = Path(__file__).with_name("config.local.json")
_local_config = json.loads(_config_path.read_text()) if _config_path.exists() else {}
DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", _local_config.get("DEVICE_TOKEN", ""))
VIEW_TOKEN = os.environ.get("VIEW_TOKEN", _local_config.get("VIEW_TOKEN", ""))

if min(len(DEVICE_TOKEN), len(VIEW_TOKEN)) < 32 or DEVICE_TOKEN == VIEW_TOKEN:
    raise RuntimeError(
        "Set DIFFERENT random DEVICE_TOKEN and VIEW_TOKEN (at least 32 chars each). "
        "Set them in config.local.json or export them before starting."
    )

# ---------------------------------------------------------------------------
# Geofence configuration
# ---------------------------------------------------------------------------
GEOFENCE_SETTINGS_PATH = Path(os.environ.get(
    "GEOFENCE_SETTINGS_PATH", str(Path(__file__).with_name("geofence.local.json"))
))


def load_geofence_settings():
    defaults = {
        "home_lat": float(os.environ.get("HOME_LAT", "29.8656")),
        "home_lng": float(os.environ.get("HOME_LNG", "77.8961")),
        "radius_m": float(os.environ.get("GEOFENCE_RADIUS_METERS", "50")),
    }
    settings = settings_store.load("geofence", GEOFENCE_SETTINGS_PATH, defaults)
    for field, low, high in [("home_lat", -90, 90), ("home_lng", -180, 180), ("radius_m", 1, 100000)]:
        value = settings.get(field)
        if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
            raise ValueError(f"Invalid saved geofence {field}")
    return {field: settings[field] for field in defaults}


geofence_settings = load_geofence_settings()

TRACKER_SETTINGS_PATH = Path(os.environ.get(
    "TRACKER_SETTINGS_PATH", str(Path(__file__).with_name("tracker.local.json"))
))


def valid_tracker_name(value):
    if not isinstance(value, str):
        return False
    return 1 <= len(value.strip()) <= 64 and not any(ord(c) < 32 or ord(c) == 127 for c in value)


def load_tracker_settings():
    settings = settings_store.load("tracker", TRACKER_SETTINGS_PATH, {"name": "animal-01"})
    if not valid_tracker_name(settings.get("name")):
        raise ValueError("Invalid saved tracker name")
    return {"name": settings["name"].strip()}


tracker_settings = load_tracker_settings()

# ---------------------------------------------------------------------------
# Known Wi-Fi APs for indoor / hybrid localisation
# Format:  "aa:bb:cc:dd:ee:ff": (lat, lon)
# Replace/extend with actual campus router BSSIDs from a site survey.
# ---------------------------------------------------------------------------
KNOWN_WIFI_APS: dict[str, tuple[float, float]] = {
    # Example IITR campus APs — replace with real BSSIDs:
    "aa:bb:cc:dd:ee:01": (29.8656, 77.8961),   # Main building AP
    "aa:bb:cc:dd:ee:02": (29.8660, 77.8970),   # Library AP
    "aa:bb:cc:dd:ee:03": (29.8650, 77.8955),   # Hostel block AP
}

# ---------------------------------------------------------------------------
# Shared state (single-process / single-worker only)
# ---------------------------------------------------------------------------
lock = threading.Lock()

state = {
    "heartbeat_at": None,
    "telemetry": None,
    "position": None,
    "history": [],          # list of recent positions (last 20)
    "geofence": None,       # geofence status object
}

_history: deque = deque(maxlen=20)
geofence_filter = GeofenceFilter()


# ---------------------------------------------------------------------------
# Utility helpers
# ---------------------------------------------------------------------------
def _haversine(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Return great-circle distance in metres between two WGS-84 points."""
    R = 6_371_000.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2) ** 2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def _send_fcm_alert(distance_m: float, tracker_name: str | None = None) -> None:
    """Fire a high-priority FCM push to topic 'geofence_alerts' (best-effort)."""
    if not FCM_ENABLED:
        return
    if tracker_name is None:
        with lock:
            tracker_name = tracker_settings["name"]
    try:
        message = fcm_messaging.Message(
            notification=fcm_messaging.Notification(
                title=f"{tracker_name} — geofence alert",
                body=f"{tracker_name} is {distance_m:.0f} m from the safe-zone centre.",
            ),
            android=fcm_messaging.AndroidConfig(priority="high"),
            topic="geofence_alerts",
        )
        fcm_messaging.send(message)
        print(f"FCM alert sent — distance {distance_m:.1f} m")
    except Exception as exc:
        print(f"FCM send failed: {exc}")


def authorize(token: str) -> None:
    supplied = request.headers.get("Authorization", "")
    expected = "Bearer " + token
    if not hmac.compare_digest(supplied.encode(), expected.encode()):
        abort(401)


def number(data: dict, key: str, low: float, high: float, optional: bool = False):
    value = data.get(key)
    if value is None and optional:
        return None
    if (
        type(value) not in (float, int)
        or not math.isfinite(value)
        or not low <= value <= high
    ):
        abort(400, description=f"Invalid {key}")
    return value


def _resolve_position(
    gps_lat: float | None,
    gps_lon: float | None,
    fix_ok: bool,
    hdop: float | None,
    wifi_aps: list | None,
) -> tuple[float | None, float | None, str]:
    """
    Hybrid Wi-Fi + GPS position resolution.

    Returns (resolved_lat, resolved_lon, loc_source)
    loc_source: "gps" | "gps+wifi" | "wifi_indoor"
    """
    if not wifi_aps:
        return gps_lat, gps_lon, "gps"

    # Find matching known APs with strong signal
    matched: list[tuple[str, int, float, float]] = []  # (bssid, rssi, lat, lon)
    for ap in wifi_aps:
        bssid = str(ap.get("bssid", "")).lower()
        rssi = ap.get("rssi")
        if not isinstance(rssi, (int, float)):
            continue
        if bssid in KNOWN_WIFI_APS and rssi > -75:
            wlat, wlon = KNOWN_WIFI_APS[bssid]
            matched.append((bssid, rssi, wlat, wlon))

    if not matched:
        return gps_lat, gps_lon, "gps"

    # Pick strongest matched AP
    matched.sort(key=lambda x: x[1], reverse=True)
    best_rssi = matched[0][1]
    best_wlat = matched[0][2]
    best_wlon = matched[0][3]

    if not fix_ok or gps_lat is None or gps_lon is None:
        # GPS unavailable → use Wi-Fi anchor
        return best_wlat, best_wlon, "wifi_indoor"

    # GPS + Wi-Fi: weighted average
    # Wi-Fi gets heavier weight when HDOP > 2.0 or RSSI is very strong (< -60 dBm)
    poor_gps = (hdop is not None and hdop > 2.0)
    strong_wifi = best_rssi > -60

    if poor_gps or strong_wifi:
        wifi_weight = 0.65
    else:
        wifi_weight = 0.30

    gps_weight = 1.0 - wifi_weight
    resolved_lat = gps_weight * gps_lat + wifi_weight * best_wlat
    resolved_lon = gps_weight * gps_lon + wifi_weight * best_wlon
    return resolved_lat, resolved_lon, "gps+wifi"


# ---------------------------------------------------------------------------
# Security headers
# ---------------------------------------------------------------------------
@app.after_request
def security_headers(response):
    if request.path.startswith("/api/"):
        response.headers["Cache-Control"] = "no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
    return response


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------
@app.get("/")
def index():
    return send_from_directory("static", "index.html")


@app.get("/map.js")
def dashboard_script():
    return send_from_directory("static", "map.js")


@app.get("/server-config.js")
def server_config_script():
    return send_from_directory("static", "server-config.js")


@app.get("/token-session.js")
def session_script():
    return send_from_directory("static", "token-session.js")


@app.get("/health")
def health():
    return jsonify({"ok": True})


@app.post("/api/telemetry")
def telemetry():

    authorize(DEVICE_TOKEN)

    if request.content_length and request.content_length > 8192:
        abort(413)

    data = request.get_json(silent=True)

    if not isinstance(data, dict) or type(data.get("fix_ok")) is not bool:
        abort(400)

    reason = data.get("reason", "")
    allowed_reasons = {"waiting", "fresh", "weak_fix", "jump_rejected", "reacquired", "stale"}
    if reason not in allowed_reasons:
        abort(400)

    if type(data.get("mpu_ok")) is not bool:
        abort(400)

    # Validate activity field (optional, default "Moving")
    activity_raw = data.get("activity", "Moving")
    if activity_raw not in ("Resting", "Grazing", "Moving"):
        activity_raw = "Moving"

    clean = {
        "fix_ok": data["fix_ok"],
        "reason": reason,
        "mpu_ok": data["mpu_ok"],
        "activity": activity_raw,
    }

    # GPS scalar fields
    for key, low, high in [
        ("satellites", 0, 100),
        ("hdop", 0, 1000),
        ("speed_kmph", 0, 2000),
        ("altitude_m", -20000, 100000),
    ]:
        clean[key] = number(data, key, low, high, optional=True)

    # BNO055 acceleration + gyroscope
    for key in ("ax", "ay", "az", "gx", "gy", "gz"):
        clean[key] = number(data, key, -1000, 1000, optional=True)

    # Wi-Fi APs (optional list)
    wifi_aps_raw = data.get("wifi_aps")
    wifi_aps: list | None = None
    if isinstance(wifi_aps_raw, list):
        wifi_aps = []
        for ap in wifi_aps_raw[:3]:
            if isinstance(ap, dict):
                bssid = ap.get("bssid", "")
                rssi = ap.get("rssi")
                if isinstance(bssid, str) and isinstance(rssi, (int, float)):
                    wifi_aps.append({"bssid": bssid[:17], "rssi": float(rssi)})
        if not wifi_aps:
            wifi_aps = None

    now = time.time()
    position = None
    loc_source = "gps"

    gps_lat: float | None = None
    gps_lon: float | None = None

    if data["fix_ok"]:
        gps_lat = number(data, "lat", -90, 90)
        gps_lon = number(data, "lon", -180, 180)
        fix_at = number(data, "fix_epoch", 1_577_836_800, now + 2)

        if now - fix_at > 10:
            clean.update(fix_ok=False, reason="stale")
        else:
            # Resolve hybrid position
            resolved_lat, resolved_lon, loc_source = _resolve_position(
                gps_lat, gps_lon, True, clean.get("hdop"), wifi_aps
            )
            position = {
                "lat": resolved_lat,
                "lon": resolved_lon,
                "gps_lat": gps_lat,
                "gps_lon": gps_lon,
                "fix_at": fix_at,
                "received_at": now,
                "reason": reason,
                "loc_source": loc_source,
            }
    elif wifi_aps:
        # No GPS fix but we have Wi-Fi — try indoor fallback
        resolved_lat, resolved_lon, loc_source = _resolve_position(
            None, None, False, None, wifi_aps
        )
        if resolved_lat is not None:
            position = {
                "lat": resolved_lat,
                "lon": resolved_lon,
                "gps_lat": None,
                "gps_lon": None,
                "fix_at": now,
                "received_at": now,
                "reason": "wifi_indoor",
                "loc_source": "wifi_indoor",
            }

    clean["loc_source"] = loc_source

    alert_distance = None
    alert_tracker_name = None
    with lock:
        geofence_status = geofence_for_position(position)
        valid_gps = bool(position and position.get("gps_lat") is not None and clean["fix_ok"])
        distance = (_haversine(position["lat"], position["lon"],
                               geofence_settings["home_lat"], geofence_settings["home_lng"])
                    if position else None)
        if geofence_filter.observe(
            distance, position["fix_at"] if position else None,
            now, geofence_settings["radius_m"], valid_gps
        ):
            alert_distance = geofence_status["distance_m"]
            alert_tracker_name = tracker_settings["name"]
        geofence_status = geofence_for_position(position)
        state["heartbeat_at"] = now
        state["telemetry"] = clean
        state["geofence"] = geofence_status

        if position and (
            state["position"] is None
            or position["fix_at"] >= state["position"]["fix_at"]
        ):
            state["position"] = position
            _history.append(
                {
                    "lat": position["lat"],
                    "lon": position["lon"],
                    "fix_at": position["fix_at"],
                    "loc_source": position["loc_source"],
                }
            )
            state["history"] = list(_history)

    if alert_distance is not None:
        _send_fcm_alert(alert_distance, alert_tracker_name)

    return jsonify({"ok": True})


def geofence_for_position(position):
    # Caller holds lock so telemetry and setting changes use one consistent boundary.
    result = dict(geofence_settings, inside=None, distance_m=None)
    distance = None
    if position is not None:
        distance = _haversine(position["lat"], position["lon"],
                              result["home_lat"], result["home_lng"])
        result.update(inside=distance <= result["radius_m"], distance_m=round(distance, 1))
    valid_gps = bool(position and position.get("gps_lat") is not None)
    result.update(geofence_filter.describe(distance, result["radius_m"], valid_gps))
    return result


@app.put("/api/geofence")
def update_geofence():
    authorize(VIEW_TOKEN)
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        abort(400, description="Expected geofence settings")
    settings = {
        "home_lat": number(data, "home_lat", -90, 90),
        "home_lng": number(data, "home_lng", -180, 180),
        "radius_m": number(data, "radius_m", 1, 100000),
    }
    with lock:
        try:
            settings_store.save("geofence", GEOFENCE_SETTINGS_PATH, settings)
        except SettingsUnavailable:
            abort(503, description="Could not save the safe zone. Please try again.")
        geofence_settings.update(settings)
        geofence_filter.reset(state["position"]["fix_at"] if state["position"] else None)
        # The next fresh telemetry packet evaluates alerts against the new boundary.
        position = state["position"]
        if position and time.time() - position["fix_at"] > 10:
            position = None
        state["geofence"] = geofence_for_position(position)
        result = dict(state["geofence"])
    return jsonify(result)


@app.put("/api/tracker")
def update_tracker():
    authorize(VIEW_TOKEN)
    data = request.get_json(silent=True)
    if not isinstance(data, dict) or not valid_tracker_name(data.get("name")):
        abort(400, description="Tracker name must contain 1–64 characters without control characters.")
    settings = {"name": data["name"].strip()}
    with lock:
        try:
            settings_store.save("tracker", TRACKER_SETTINGS_PATH, settings)
        except SettingsUnavailable:
            abort(503, description="Could not save the tracker name. Please try again.")
        tracker_settings.update(settings)
        result = dict(tracker_settings)
    return jsonify(result)


@app.get("/api/latest")
def latest():
    authorize(VIEW_TOKEN)

    with lock:
        result = copy.deepcopy(state)
        result["tracker"] = dict(tracker_settings)
        position = result["position"]
        if position and time.time() - position["fix_at"] > 10:
            position = None
        result["geofence"] = geofence_for_position(position)

    result["server_now"] = time.time()
    return jsonify(result)


if __name__ == "__main__":
    app.run(
        host="0.0.0.0",
        port=int(os.environ.get("PORT", _local_config.get("PORT", 8000))),
        debug=False,
    )
