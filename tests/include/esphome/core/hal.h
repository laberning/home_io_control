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
  static uint32_t micros_counter = 0;
  micros_counter++;
  if (micros_counter >= 1000) {
    micros_counter = 0;
    counter++;
  }
  return counter;
}

inline void delay(uint32_t ms) { (void) ms; }

inline void delayMicroseconds(uint32_t us) { (void) us; }

}  // namespace esphome
