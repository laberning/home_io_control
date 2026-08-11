#include <cstdint>

#include <esphome/core/preferences.h>
#include <esphome/core/application.h>

namespace esphome {

namespace test_preferences {

std::map<uint32_t, std::vector<uint8_t>> &committed() {
  static std::map<uint32_t, std::vector<uint8_t>> store;
  return store;
}

std::map<uint32_t, std::vector<uint8_t>> &staged() {
  static std::map<uint32_t, std::vector<uint8_t>> store;
  return store;
}

void simulate_reboot() { staged().clear(); }

void wipe() {
  staged().clear();
  committed().clear();
}

}  // namespace test_preferences

// Global objects. Unlike the previous no-op stub, `global_preferences` points at a real instance:
// the sequence store dereferences it during setup, and a null here would only ever surface as a
// crash in whichever test happened to run first.
ESPPreferences global_preferences_instance;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
ESPPreferences *global_preferences = &global_preferences_instance;
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
