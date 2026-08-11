#pragma once

#include <cstdint>
#include <vector>
#include "gpio.h"

namespace esphome {

/// Host-only recording of blocking delays. `delay()` cannot actually sleep here and `millis()` is
/// a fake counter, so timing-sensitive code would otherwise be untestable: a burst that forgot its
/// inter-frame gap entirely would look identical to one that honoured it. Recording the requested
/// durations lets a test assert the cadence that was *asked for*, which is the part this codebase
/// controls — the part it does not control is the scheduler's, and no host test can speak to that.
namespace test_hal {

inline std::vector<uint32_t> &recorded_delays() {
  static std::vector<uint32_t> delays;
  return delays;
}

inline void reset_delays() { recorded_delays().clear(); }

}  // namespace test_hal

inline uint32_t micros() {
  static uint32_t counter = 0;
  return counter++;
}

inline uint32_t millis() {
  static uint32_t counter = 0;
  return ++counter;
}

inline void delay(uint32_t ms) { test_hal::recorded_delays().push_back(ms); }

inline void delayMicroseconds(uint32_t us) { (void) us; }

}  // namespace esphome
