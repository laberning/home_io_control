#pragma once

#include <cstdint>
#include "gpio.h"

namespace esphome {

inline uint32_t micros() {
  static uint32_t counter = 0;
  return counter++;
}

inline uint32_t millis() {
  static uint32_t counter = 0;
  return ++counter;
}

inline void delay(uint32_t ms) { (void) ms; }

inline void delayMicroseconds(uint32_t us) { (void) us; }

}  // namespace esphome
