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

  // Self-keyed overload, mirroring the real Scheduler::set_timeout(const void *self, ...): no
  // Component is recorded, so (unlike the overload above) this is not subject to a failed
  // component being skipped. There is no Component to store the callback on, so it is recorded
  // here on the Scheduler stub instead, keyed by `self` for cancel_timeout()/inspection.
  void set_timeout(const void *self, uint32_t timeout, std::function<void()> &&func) {
    last_self_timeout_self_ = self;
    last_self_timeout_ms_ = timeout;
    last_self_timeout_callback_ = std::move(func);
  }
  bool cancel_timeout(const void *self) {
    if (self == nullptr || self != last_self_timeout_self_)
      return false;
    last_self_timeout_self_ = nullptr;
    last_self_timeout_callback_ = nullptr;
    return true;
  }

  // Test helpers for verifying self-keyed set_timeout() calls.
  const void *last_self_timeout_self_{nullptr};
  uint32_t last_self_timeout_ms_{0};
  std::function<void()> last_self_timeout_callback_;
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

  // Counts calls instead of actually restarting the process, so tests can assert a code path
  // reaches the safe_reboot() invariant (see the LR1121 firmware-update ground rules: every
  // bootloader excursion must end in either radio_->init() or App.safe_reboot()) without tearing
  // down the test binary.
  uint32_t safe_reboot_calls{0};
  void safe_reboot() { this->safe_reboot_calls++; }
};

extern Application App;

}  // namespace esphome
