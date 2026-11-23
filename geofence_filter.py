"""Confirmation and hysteresis for one tracker's geofence alerts."""


class GeofenceFilter:
    required_fixes = 3
    max_gap_seconds = 10

    def __init__(self):
        self.reset()

    def reset(self, last_fix_epoch=None):
        self.outside_count = 0
        self.breached = False
        self.last_fix_epoch = last_fix_epoch

    @staticmethod
    def reentry_buffer(radius):
        return min(10.0, radius * 0.2)

    def observe(self, distance, epoch, now, radius, valid_gps):
        # Wi-Fi anchors and stale/missing/invalid GPS cannot confirm a breach.
        if not valid_gps or distance is None or epoch is None or now - epoch > 10:
            self.outside_count = 0
            return False
        if self.last_fix_epoch is not None:
            if epoch <= self.last_fix_epoch:
                return False  # Duplicate or reordered upload is not another fix.
            if epoch - self.last_fix_epoch > self.max_gap_seconds:
                self.outside_count = 0
        self.last_fix_epoch = epoch
        if distance <= radius:
            self.outside_count = 0
            if distance <= radius - self.reentry_buffer(radius):
                self.breached = False
            return False
        if self.breached:
            return False
        self.outside_count += 1
        if self.outside_count >= self.required_fixes:
            self.breached = True
            return True
        return False

    def describe(self, distance, radius, valid_gps):
        if distance is None:
            status = 'unknown'
        elif not valid_gps:
            status = 'wifi_only'
        elif self.breached:
            status = 'breached' if distance > radius else 'reentry_buffer'
        elif distance > radius:
            status = 'pending'
        else:
            status = 'inside'
        return {
            'alert_state': status,
            'outside_count': self.outside_count if valid_gps else 0,
            'required_fixes': self.required_fixes,
            'reentry_buffer_m': self.reentry_buffer(radius),
            'reentry_radius_m': radius - self.reentry_buffer(radius),
        }
