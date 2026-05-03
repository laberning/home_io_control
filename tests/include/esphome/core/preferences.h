#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace esphome {

uint32_t fnv1_hash(const char *str);

template<typename T> class Preference {
 public:
  void save(const T *data) { (void) data; }
  void save(const T &data) { (void) data; }
  bool load(T *data) {
    (void) data;
    return false;
  }
};

class GlobalPreferences {
 public:
  template<typename T> Preference<T> make_preference(uint32_t) { return {}; }
  void sync() {}
};

extern GlobalPreferences *global_preferences;

}  // namespace esphome
