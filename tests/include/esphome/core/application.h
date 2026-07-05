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
  void feed_wdt() {}
};

extern Application App;

}  // namespace esphome
