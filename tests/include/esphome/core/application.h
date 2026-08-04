#pragma once

#include "component.h"

#include <functional>

namespace esphome {

/// Minimal host stub of ESPHome's Scheduler. Records the most recent timeout onto the
/// target component, mirroring the Component::set_timeout stub so tests can inspect
/// scheduled callbacks regardless of which entry point the production code used.
class Scheduler {
 public:
  void set_timeout(Component *component, const char *name, uint32_t timeout, std::function<void()> &&func) {
    if (component != nullptr) {
      component->last_timeout_name_ = name != nullptr ? name : "";
      component->last_timeout_ms_ = timeout;
      component->last_timeout_callback_ = std::move(func);
    }
  }
};

class Application {
 public:
  Scheduler scheduler;
  // Counts calls rather than being a pure no-op so tests can assert that a long blocking wait
  // actually feeds the watchdog, not just that it eventually returns. Global and monotonic —
  // callers snapshot the count before their wait and assert on the delta, since unrelated tests
  // may also drive code paths that feed.
  uint32_t feed_wdt_calls{0};
  void feed_wdt() { this->feed_wdt_calls++; }
};

extern Application App;

}  // namespace esphome
