import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import app as backend


class GeofenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / 'geofence.json'
        self.path_patch = patch.object(backend, 'GEOFENCE_SETTINGS_PATH', self.path)
        self.path_patch.start()
        self.alert = patch.object(backend, '_send_fcm_alert').start()
        self.client = backend.app.test_client()
        self.viewer = {'Authorization': 'Bearer ' + backend.VIEW_TOKEN}
        self.device = {'Authorization': 'Bearer ' + backend.DEVICE_TOKEN}
        backend.geofence_settings.update(home_lat=0, home_lng=0, radius_m=50)
        backend.state.update(heartbeat_at=None, telemetry=None, position=None, history=[], geofence=None)
        backend._history.clear()
        backend.geofence_filter.reset()
        self.clock = 1800000000.0
        patch.object(backend.time, 'time', side_effect=lambda: self.clock).start()

    def tearDown(self):
        patch.stopall()
        self.directory.cleanup()

    def save(self, **values):
        data = dict(home_lat=0, home_lng=0, radius_m=50)
        data.update(values)
        return self.client.put('/api/geofence', headers=self.viewer, json=data)

    def upload(self, lat, lon, epoch=None, **overrides):
        self.clock += 2
        payload = dict(fix_ok=True, reason='fresh', mpu_ok=True,
                       lat=lat, lon=lon, fix_epoch=self.clock if epoch is None else epoch)
        payload.update(overrides)
        return self.client.post('/api/telemetry', headers=self.device, json=payload)

    def test_requires_viewer_authorization(self):
        for headers in [{}, self.device]:
            self.assertEqual(self.client.put('/api/geofence', headers=headers,
                                            json=dict(home_lat=0, home_lng=0, radius_m=10)).status_code, 401)
        self.assertFalse(self.path.exists())

    def test_validation_does_not_change_settings(self):
        before = dict(backend.geofence_settings)
        for data in [dict(radius_m=0), dict(radius_m=-1), dict(radius_m=100001),
                     dict(home_lat=91), dict(home_lng=-181), dict(radius_m=True),
                     dict(home_lat='1'), dict(radius_m=float('nan'))]:
            self.assertEqual(self.save(**data).status_code, 400)
        self.assertEqual(backend.geofence_settings, before)
        self.assertFalse(self.path.exists())

    def test_save_persists_and_loads_after_restart(self):
        response = self.save(home_lat=12.5, home_lng=77, radius_m=125)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(backend.load_geofence_settings(), dict(home_lat=12.5, home_lng=77, radius_m=125))
        self.assertIsNone(response.json['inside'])
        self.assertEqual(self.client.get('/api/latest', headers=self.viewer).json['geofence']['radius_m'], 125)

    def test_three_distinct_fixes_and_reentry_hysteresis(self):
        self.upload(0, .001)
        first_epoch = self.clock
        self.assertEqual(backend.state['geofence']['alert_state'], 'pending')
        self.upload(0, .001, epoch=first_epoch)
        self.assertEqual(backend.state['geofence']['outside_count'], 1)
        self.upload(0, .001)
        self.assertEqual(self.alert.call_count, 0)
        self.upload(0, .001)
        self.assertEqual(self.alert.call_count, 1)
        self.upload(0, .001)
        self.upload(0, .000405)  # About 45 m: inside radius 50, outside reset radius 40.
        self.assertEqual(backend.state['geofence']['alert_state'], 'reentry_buffer')
        for _ in range(3): self.upload(0, .001)
        self.assertEqual(self.alert.call_count, 1)
        self.upload(0, .00030)  # About 33 m: definitely back inside.
        for _ in range(3): self.upload(0, .001)
        self.assertEqual(self.alert.call_count, 2)

    def test_invalid_inside_and_long_gaps_break_confirmation(self):
        self.upload(0, .001)
        self.upload(0, .001, fix_ok=False, reason='jump_rejected')
        self.upload(0, .001)
        self.assertEqual(backend.state['geofence']['outside_count'], 1)
        self.upload(0, 0)
        self.upload(0, .001)
        self.assertEqual(backend.state['geofence']['outside_count'], 1)
        self.clock += 20
        self.upload(0, .001)
        self.assertEqual(backend.state['geofence']['outside_count'], 1)
        self.assertEqual(self.alert.call_count, 0)

    def test_new_boundary_resets_confirmation(self):
        for _ in range(2): self.upload(0, .001)
        self.save(radius_m=200)
        self.upload(0, .001)
        self.assertTrue(backend.state['geofence']['inside'])
        self.save(radius_m=50)
        for _ in range(2): self.upload(0, .001)
        self.assertEqual(self.alert.call_count, 0)
        self.upload(0, .001)
        self.assertEqual(self.alert.call_count, 1)

    def test_wifi_only_and_stale_fixes_do_not_confirm(self):
        for _ in range(3):
            self.upload(0, .001, fix_ok=False, reason='waiting',
                        wifi_aps=[dict(bssid='aa:bb:cc:dd:ee:01', rssi=-50)])
        self.assertEqual(self.alert.call_count, 0)
        self.assertEqual(backend.state['geofence']['alert_state'], 'wifi_only')
        self.upload(0, .001)
        self.upload(0, .001, epoch=self.clock-20)
        self.assertEqual(backend.state['geofence']['outside_count'], 0)
        self.assertEqual(self.alert.call_count, 0)

    def test_failed_write_does_not_apply_and_stale_position_is_unknown(self):
        with patch.object(backend.os, 'replace', side_effect=OSError('disk unavailable')):
            self.assertEqual(self.save(radius_m=80).status_code, 503)
        self.assertEqual(backend.geofence_settings['radius_m'], 50)
        self.upload(0, 0)
        backend.state['position']['fix_at'] = time.time() - 60
        result = self.client.get('/api/latest', headers=self.viewer).json['geofence']
        self.assertIsNone(result['inside'])
        self.assertIsNone(result['distance_m'])


if __name__ == '__main__':
    unittest.main()
