"""Persistent settings: atomic local JSON for development, Firestore for hosting."""
import json
import os
import tempfile
from pathlib import Path


class SettingsUnavailable(Exception):
    pass


class SettingsStore:
    def __init__(self, backend='local', client=None, tracker_id='animal-01'):
        if backend not in ('local', 'firestore'):
            raise ValueError('SETTINGS_BACKEND must be local or firestore')
        if backend == 'firestore' and client is None:
            raise ValueError('Firestore settings require a Firebase client')
        self.backend = backend
        self.client = client
        self.tracker_id = tracker_id

    def document(self, section):
        return self.client.collection('trackers').document(self.tracker_id).collection('settings').document(section)

    def load(self, section, path, defaults):
        if self.backend == 'firestore':
            snapshot = self.document(section).get(timeout=10)
            return snapshot.to_dict() if snapshot.exists else dict(defaults)
        path = Path(path)
        return json.loads(path.read_text()) if path.exists() else dict(defaults)

    def save(self, section, path, value):
        if self.backend == 'firestore':
            try:
                self.document(section).set(value, timeout=10)
            except Exception as error:
                raise SettingsUnavailable('Cloud settings storage unavailable') from error
            return
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', dir=Path(path).parent,
                                             prefix='.settings-', delete=False) as output:
                temporary = Path(output.name)
                json.dump(value, output)
            os.replace(temporary, path)
        except OSError as error:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
            raise SettingsUnavailable('Local settings storage unavailable') from error
