import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import app as backend


class TrackerNameTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        patch.object(backend, 'TRACKER_SETTINGS_PATH', Path(self.directory.name) / 'tracker.json').start()
        patch.dict(backend.tracker_settings, {'name': 'animal-01'}, clear=True).start()
        self.send = patch.object(backend.fcm_messaging, 'send').start()
        patch.object(backend, 'FCM_ENABLED', True).start()
        self.client = backend.app.test_client()
        self.viewer = {'Authorization': 'Bearer ' + backend.VIEW_TOKEN}
        self.device = {'Authorization': 'Bearer ' + backend.DEVICE_TOKEN}
        backend.geofence_filter.reset()

    def tearDown(self):
        patch.stopall()
        self.directory.cleanup()

    def save(self, name, headers=None):
        return self.client.put('/api/tracker', headers=self.viewer if headers is None else headers,
                               json={'name': name})

    def test_default_and_persistence(self):
        self.assertEqual(backend.load_tracker_settings()['name'], 'animal-01')
        self.assertEqual(self.save('  Daisy  ').status_code, 200)
        self.assertEqual(backend.load_tracker_settings()['name'], 'Daisy')
        self.assertEqual(self.client.get('/api/latest', headers=self.viewer).json['tracker']['name'], 'Daisy')

    def test_auth_validation_and_failed_storage(self):
        for headers in [{}, self.device]:
            self.assertEqual(self.save('Daisy', headers).status_code, 401)
        for value in ['', '   ', 'x'*65, None, True, 'line\nbreak', 'name\x00']:
            self.assertEqual(self.save(value).status_code, 400)
        with patch.object(backend.os, 'replace', side_effect=OSError('disk full')):
            self.assertEqual(self.save('Daisy').status_code, 503)
        self.assertEqual(backend.tracker_settings['name'], 'animal-01')

    def test_notification_contains_saved_name_and_default(self):
        backend._send_fcm_alert(75)
        message = self.send.call_args.args[0]
        self.assertIn('animal-01', message.notification.title)
        self.assertIn('animal-01', message.notification.body)
        self.save('Daisy 🐄')
        backend._send_fcm_alert(75)
        message = self.send.call_args.args[0]
        self.assertIn('Daisy 🐄', message.notification.title)
        self.assertIn('Daisy 🐄', message.notification.body)
        self.assertIn('75 m', message.notification.body)

    def test_rename_does_not_rearm_breach(self):
        backend.geofence_filter.breached = True
        self.save('Daisy')
        self.assertTrue(backend.geofence_filter.breached)
        self.send.assert_not_called()

    def test_notification_uses_name_at_event_time(self):
        self.save('New name')
        backend._send_fcm_alert(100, 'Name when breach occurred')
        self.assertIn('Name when breach occurred', self.send.call_args.args[0].notification.title)


if __name__ == '__main__':
    unittest.main()
