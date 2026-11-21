import copy
import hmac
import math
import os
import threading
import time

from flask import Flask, jsonify, request, send_from_directory, abort

app = Flask(__name__, static_folder="static")
app.config["MAX_CONTENT_LENGTH"] = 4096

DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", "")
VIEW_TOKEN = os.environ.get("VIEW_TOKEN", "")

# Both tokens must be different and sufficiently long.
if (
    min(len(DEVICE_TOKEN), len(VIEW_TOKEN)) < 32
    or DEVICE_TOKEN == VIEW_TOKEN
):
    raise RuntimeError(
        "Set DIFFERENT random DEVICE_TOKEN and VIEW_TOKEN "
        "(at least 32 characters)."
    )

lock = threading.Lock()

# RAM-only demonstration: use exactly ONE worker.
state = {
    "heartbeat_at": None,
    "telemetry": None,
    "position": None
}


def authorize(token):
    supplied = request.headers.get("Authorization", "")
    expected = "Bearer " + token

    if not hmac.compare_digest(
        supplied.encode(),
        expected.encode()
    ):
        abort(401)


def number(data, key, low, high, optional=False):
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


@app.after_request
def security_headers(response):
    if request.path.startswith("/api/"):
        response.headers["Cache-Control"] = "no-store"

    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = (
        "strict-origin-when-cross-origin"
    )

    return response


@app.get("/")
def index():
    return send_from_directory("static", "index.html")


@app.get("/health")
def health():
    return jsonify({"ok": True})


@app.post("/api/telemetry")
def telemetry():
    authorize(DEVICE_TOKEN)

    if request.content_length and request.content_length > 4096:
        abort(413)

    data = request.get_json(silent=True)

    if (
        not isinstance(data, dict)
        or type(data.get("fix_ok")) is not bool
    ):
        abort(400)

    reason = data.get("reason", "")

    allowed_reasons = {
        "waiting",
        "fresh",
        "weak_fix",
        "jump_rejected",
        "reacquired",
        "stale"
    }

    if reason not in allowed_reasons:
        abort(400)

    # Kept as mpu_ok for compatibility with the existing dashboard.
    # It now represents BNO055 sensor availability.
    if type(data.get("mpu_ok")) is not bool:
        abort(400)

    clean = {
        "fix_ok": data["fix_ok"],
        "reason": reason,
        "mpu_ok": data["mpu_ok"]
    }

    # GPS telemetry.
    for key, low, high in [
        ("satellites", 0, 100),
        ("hdop", 0, 1000),
        ("speed_kmph", 0, 2000),
        ("altitude_m", -20000, 100000)
    ]:
        clean[key] = number(
            data, key, low, high, optional=True
        )

    # BNO055 acceleration and gyroscope readings.
    for key in ("ax", "ay", "az", "gx", "gy", "gz"):
        clean[key] = number(
            data, key, -1000, 1000, optional=True
        )

    now = time.time()
    position = None

    if data["fix_ok"]:
        lat = number(data, "lat", -90, 90)
        lon = number(data, "lon", -180, 180)

        fix_at = number(
            data,
            "fix_epoch",
            1577836800,
            now + 2
        )

        # Reject delayed GPS measurements.
        if now - fix_at > 10:
            clean.update(
                fix_ok=False,
                reason="stale"
            )
        else:
            position = {
                "lat": lat,
                "lon": lon,
                "fix_at": fix_at,
                "received_at": now,
                "reason": reason
            }

    with lock:
        state["heartbeat_at"] = now
        state["telemetry"] = clean

        if position and (
            state["position"] is None
            or position["fix_at"] > state["position"]["fix_at"]
        ):
            state["position"] = position

    return jsonify({"ok": True})


@app.get("/api/latest")
def latest():
    authorize(VIEW_TOKEN)

    with lock:
        result = copy.deepcopy(state)

    result["server_now"] = time.time()

    return jsonify(result)


if __name__ == "__main__":
    # Local testing only.
    # Production deployment uses Gunicorn.
    app.run(
        host="0.0.0.0",
        port=int(os.environ.get("PORT", "8000")),
        debug=False
    )
