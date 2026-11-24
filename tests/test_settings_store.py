import copy
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock

from settings_store import SettingsStore, SettingsUnavailable


class SettingsStoreTests(unittest.TestCase):
    def cloud_store(self):
        saved = {}
        client = MagicMock()
        documents = {}
        def document(section):
            if section not in documents:
                doc = MagicMock()
                doc.get.side_effect = lambda **kwargs: SimpleNamespace(
                    exists=section in saved, to_dict=lambda: copy.deepcopy(saved[section]))
                doc.set.side_effect = lambda value, **kwargs: saved.__setitem__(section, copy.deepcopy(value))
                documents[section] = doc
            return documents[section]
        client.collection.return_value.document.return_value.collection.return_value.document.side_effect = document
        return SettingsStore('firestore', client, 'animal-01'), saved, documents

    def test_cloud_settings_survive_new_store_without_local_file(self):
        store, saved, documents = self.cloud_store()
        path = Path('/not-a-local-settings-file.json')
        self.assertEqual(store.load('tracker', path, {'name': 'animal-01'}), {'name': 'animal-01'})
        store.save('tracker', path, {'name': 'Daisy'})
        restarted = SettingsStore('firestore', store.client, 'animal-01')
        self.assertEqual(restarted.load('tracker', path, {}), {'name': 'Daisy'})
        store.save('geofence', path, {'radius_m': 75})
        self.assertEqual(restarted.load('geofence', path, {}), {'radius_m': 75})
        self.assertEqual(restarted.load('tracker', path, {}), {'name': 'Daisy'})
        self.assertFalse(path.exists())

    def test_cloud_failure_does_not_silently_save_to_ephemeral_disk(self):
        store, saved, documents = self.cloud_store()
        store.load('tracker', Path('/unused'), {})
        documents['tracker'].set.side_effect = RuntimeError('offline')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'tracker.json'
            with self.assertRaises(SettingsUnavailable):
                store.save('tracker', path, {'name': 'Daisy'})
            self.assertFalse(path.exists())

    def test_local_json_is_atomic_private_and_compatible(self):
        store = SettingsStore()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'tracker.json'
            store.save('tracker', path, {'name': 'Daisy'})
            self.assertEqual(store.load('tracker', path, {}), {'name': 'Daisy'})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(list(path.parent.glob('.settings-*')), [])

    def test_invalid_backend_or_missing_cloud_client_fails(self):
        with self.assertRaises(ValueError): SettingsStore('unknown')
        with self.assertRaises(ValueError): SettingsStore('firestore')
