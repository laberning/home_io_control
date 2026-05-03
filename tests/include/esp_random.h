#pragma once

#include <cstdint>

// Stub for esp_random() used in host tests
inline uint32_t esp_random() {
  // Simple LCG for testing — not cryptographic, only for deterministic test vectors
  static uint32_t seed = 0x12345678;
  seed = seed * 1664525 + 1013904223;
  return seed;
}
