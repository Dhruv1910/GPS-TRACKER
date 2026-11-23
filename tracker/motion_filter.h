#pragma once
#include <cmath>
#include <stdint.h>

// Quiet motion is independent of the posture/activity label. All units are SI.
class MotionFilter {
 public:
  void update(float acceleration, float rotation, uint32_t now) {
    bool quiet = std::isfinite(acceleration) && std::isfinite(rotation) &&
                 acceleration < 0.3f && rotation < 0.08f;
    if (!quiet || !quiet_ || !sampled_ || uint32_t(now - sampledAt_) > 250) quietSince_ = now;
    quiet_ = quiet;
    sampled_ = true;
    sampledAt_ = now;
  }
  void reset() { sampled_ = false; quiet_ = false; }
  bool quietFor(uint32_t now, uint32_t duration) const {
    return sampled_ && quiet_ && uint32_t(now - sampledAt_) <= 250 &&
           uint32_t(now - quietSince_) >= duration;
  }
  bool stationary(uint32_t now) const { return quietFor(now, 2000); }
  bool protectPosition(uint32_t now, bool speedValid, double previousSpeedMps) const {
    return quietFor(now, 500) && (speedValid ? previousSpeedMps <= 0.5 : stationary(now));
  }
 private:
  bool sampled_ = false, quiet_ = false;
  uint32_t sampledAt_ = 0, quietSince_ = 0;
};
