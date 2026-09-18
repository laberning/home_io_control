#include <cstdint>

#include <esphome/core/preferences.h>
#include <esphome/core/application.h>

namespace esphome {

// Component::set_timeout/set_interval/cancel_*/~Component() are declared in component.h but
// defined here rather than there: they forward to App.scheduler (application.h), and component.h
// can't include application.h itself — application.h includes component.h for the Component type
// its Scheduler stores, so that direction would be a cycle.

Component::~Component() { test_scheduler::purge_owner(this); }

void Component::set_timeout(const char *name, uint32_t timeout, std::function<void()> &&f) {
  App.scheduler.set_timeout(this, name, timeout, std::move(f));
}

void Component::set_timeout(uint32_t id, uint32_t timeout, std::function<void()> &&f) {
  App.scheduler.set_timeout(this, id, timeout, std::move(f));
}

void Component::set_interval(const char *name, uint32_t interval, std::function<void()> &&f) {
  App.scheduler.set_interval(this, name, interval, std::move(f));
}

bool Component::cancel_timeout(const char *name) { return App.scheduler.cancel_timeout(this, name); }

bool Component::cancel_timeout(uint32_t id) { return App.scheduler.cancel_timeout(this, id); }

bool Component::cancel_interval(const char *name) { return App.scheduler.cancel_interval(this, name); }

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
