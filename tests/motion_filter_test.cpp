#include "../tracker/motion_filter.h"
#include <cassert>
#include <limits>

int main() {
  MotionFilter motion;
  assert(!motion.stationary(0));
  // Low previous speed protects position before the two-second Resting label.
  for (unsigned t = 0; t <= 500; t += 50) motion.update(0.05f, 0.01f, t);
  assert(!motion.stationary(500));
  assert(motion.protectPosition(500, true, 0.1));
  assert(!motion.protectPosition(500, true, 2.0));
  for (unsigned t = 550; t <= 2000; t += 50) motion.update(0.05f, 0.01f, t);
  assert(motion.stationary(2000));
  assert(!motion.protectPosition(2000, true, 2.0)); // Quiet IMU is not zero velocity.
  assert(motion.protectPosition(2000, false, 0));
  // Real acceleration or turning releases the anchor immediately.
  motion.update(1.0f, 0, 2050);
  assert(!motion.protectPosition(2050, true, 0));
  motion.update(0, 0.2f, 2100);
  assert(!motion.protectPosition(2100, true, 0));
  for (unsigned t = 2150; t <= 4150; t += 50) motion.update(0.05f, 0.01f, t);
  assert(motion.stationary(4150));
  assert(!motion.stationary(4500)); // Missing/stale sensor readings fail open.
  motion.update(0, 0, 4500);
  assert(!motion.stationary(4500)); // A sampling gap requires a new quiet interval.
  motion.update(std::numeric_limits<float>::quiet_NaN(), 0, 4550);
  assert(!motion.stationary(4550));
  motion.reset();
  assert(!motion.stationary(4550));
  // Millisecond counter wrap does not break a quiet interval.
  for (uint32_t t = 0; t <= 2100; t += 50) motion.update(0, 0, UINT32_MAX-1000+t);
  assert(motion.stationary(UINT32_MAX-1000+2100));
}
