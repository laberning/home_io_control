#pragma once

/// @file preferences.h
/// @brief Host stub for ESPHome's persistent-preferences API.
///
/// Deliberately models ESP32's *staged* write behaviour rather than pretending a save is
/// immediately durable: `ESP32PreferenceBackend::save()` only appends to a pending list, and
/// nothing reaches NVS until `sync()` commits it (esphome/components/esp32/preferences.cpp). A
/// stub whose `save()` were instantly durable would let code that forgets `sync()` pass every
/// host test and then silently lose its writes on real hardware — which, for the rolling 1W
/// sequence counter, is the exact failure the counter exists to prevent.
///
/// So: `save()` stages, `sync()` commits, and simulate_reboot() drops whatever was staged but
/// never committed. Signatures mirror esphome/core/preference_backend.h; keep them in step.

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace esphome {

uint32_t fnv1_hash(const char *str);

namespace test_preferences {

/// Committed (durable) blobs, keyed by preference type hash. Survives simulate_reboot().
std::map<uint32_t, std::vector<uint8_t>> &committed();
/// Staged blobs awaiting sync(). Discarded by simulate_reboot().
std::map<uint32_t, std::vector<uint8_t>> &staged();
/// Drop staged-but-unsynced writes, keeping committed ones — a power cut.
void simulate_reboot();
/// Wipe both stores — a virgin board.
void wipe();

}  // namespace test_preferences

/// Mirrors esphome::ESPPreferenceObject. Reads see staged writes (as ESP32's backend does, which
/// is why a missing sync() is invisible until a reboot); only sync() makes them durable.
class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  explicit ESPPreferenceObject(uint32_t key) : key_(key), valid_(true) {}

  template<typename T> bool save(const T *src) {
    if (!this->valid_)
      return false;
    std::vector<uint8_t> blob(sizeof(T));
    std::memcpy(blob.data(), src, sizeof(T));
    test_preferences::staged()[this->key_] = std::move(blob);
    return true;
  }

  template<typename T> bool load(T *dest) {
    if (!this->valid_)
      return false;
    auto &pending = test_preferences::staged();
    auto staged_it = pending.find(this->key_);
    if (staged_it != pending.end() && staged_it->second.size() == sizeof(T)) {
      std::memcpy(dest, staged_it->second.data(), sizeof(T));
      return true;
    }
    auto &durable = test_preferences::committed();
    auto committed_it = durable.find(this->key_);
    if (committed_it == durable.end() || committed_it->second.size() != sizeof(T))
      return false;
    std::memcpy(dest, committed_it->second.data(), sizeof(T));
    return true;
  }

 private:
  uint32_t key_{0};
  bool valid_{false};
};

/// Mirrors esphome::ESPPreferences (the `Preferences` alias upstream).
class ESPPreferences {
 public:
  template<typename T> ESPPreferenceObject make_preference(uint32_t type, bool in_flash = false) {
    (void) in_flash;
    return ESPPreferenceObject(type);
  }
  bool sync() {
    for (auto &entry : test_preferences::staged())
      test_preferences::committed()[entry.first] = entry.second;
    test_preferences::staged().clear();
    return true;
  }
  bool reset() {
    test_preferences::wipe();
    return true;
  }
};

extern ESPPreferences *global_preferences;

}  // namespace esphome
