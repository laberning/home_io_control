#include <cstdint>

#include <esphome/core/preferences.h>
#include <esphome/core/application.h>

namespace esphome {

// Global objects
GlobalPreferences *global_preferences = nullptr;
Application App;

// FNV-1 hash function
uint32_t fnv1_hash(const char *str) {
  uint32_t hash = 2166136261u;
  while (*str) {
    hash ^= static_cast<uint8_t>(*str++);
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace esphome
