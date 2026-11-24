# Online tracker backend

The prepared deployment uses a Render Python web service for the Flask API,
Firebase Cloud Messaging for notifications, and Firestore for saved settings.
The Mac is not part of the deployed data path. The tracker still requires an
internet connection (campus IoT Wi-Fi or a hotspot).

## Deployment configuration

- Runtime: Python 3.12 (the latest patch is selected by `.python-version`).
- Build: `pip install -r requirements.txt`.
- Start: `gunicorn --workers 1 --threads 4 --bind 0.0.0.0:$PORT --timeout 120 app:app`.
- Health check: `/health`.
- Keep one worker: live telemetry, history and breach confirmation are in memory.
- Use the existing device/view tokens to avoid changing login credentials.
- Set `SETTINGS_BACKEND=firestore`, `TRACKER_ID=animal-01`, `REQUIRE_FCM=true`,
  and `FIREBASE_CREDENTIALS_PATH=/etc/secrets/firebase-admin.json`.
- Add the private Firebase Admin JSON as a Render secret file named
  `firebase-admin.json`. Never commit it to Git or put it in the Android app.

The `render.yaml` file describes a free service. An existing service can be
configured with the same values; importing the Blueprint is not required.
Do not upgrade to paid compute or add billing without the user's approval.

## Persistent settings

Create the default Firestore database in the existing Animal Tracker Firebase
project, using Standard edition and production/locked rules. No phone/browser
Firestore access is needed: the Flask server uses its Firebase Admin credential.
The settings are stored under:

- `trackers/animal-01/settings/geofence`
- `trackers/animal-01/settings/tracker`

The tracker ID is stable even when the displayed tracker name changes. Settings
are read at startup, cached in memory, and written only when the user saves a
name or geofence. Telemetry is not written to Firestore on every packet.
A failed cloud settings write returns an error; it does not silently fall back
to Render's temporary filesystem. Existing local JSON files remain the default
storage for local development.

After deployment, restore the intended centre/radius/name using the authenticated
app or API. They will persist across Render restarts. Live position/history and
pending breach counters reset after a server restart until new telemetry arrives.

## Client cutover

After Render provides the verified HTTPS URL:

1. Put that URL (without a trailing slash) in `static/server-config.js` as
   `window.FIELD_TRACKER_API_BASE_URL`.
2. Update the private `tracker/secrets.h` API_URL to that HTTPS host plus
   `/api/telemetry`, disable `ALLOW_LAN_HTTP`, and install the appropriate public
   CA root certificate in `ROOT_CA`. Keep certificate verification enabled.
3. Rebuild the Android app with `npm run cap:sync`, then build/export the APK.
4. Re-upload the firmware, install the updated APK, and verify live telemetry
   with the phone on a different internet connection from the tracker.
5. Confirm named notifications and the three-fix geofence trigger. Finally stop
   Flask on the Mac and verify the online system continues to work.

## Free-hosting limits

Render free services can sleep after 15 minutes without inbound traffic and may
take about a minute to wake. This can delay reconnection after long idle periods.
The local filesystem is temporary; that is why hosted settings use Firestore.
The free deployment is for demonstrations, not a guarantee of always-on alerts.
