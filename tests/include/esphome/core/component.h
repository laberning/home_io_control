#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {

class Component {
 public:
  // Out-of-line (defined in stubs.cpp, which already includes application.h): these forward to
  // App.scheduler.*(this, ...), the same way real ESPHome's component.cpp does. component.h can't
  // include application.h itself — application.h includes this header for the Component type its
  // Scheduler stores, so that direction would be a cycle.
  virtual ~Component();
  void set_timeout(const char *name, uint32_t timeout, std::function<void()> &&f);
  /// Numeric-id sibling of the name overload above, for a caller whose timer key isn't a
  /// static-lifetime string (e.g. hub_status.cpp's per-device remote-activity poll — see
  /// decisions::remote_poll_timer_id()). Real ESPHome's `const char *` overload stores the
  /// caller's pointer rather than copying it; this overload exists precisely for callers who
  /// can't promise that lifetime.
  void set_timeout(uint32_t id, uint32_t timeout, std::function<void()> &&f);
  void set_interval(const char *name, uint32_t interval, std::function<void()> &&f);
  bool cancel_timeout(const char *name);
  bool cancel_timeout(uint32_t id);
  bool cancel_interval(const char *name);

  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }

  // Test helpers mirroring the most recent set_timeout()/set_interval() call on this component —
  // written on every call, regardless of whether it used the name or the numeric-id overload
  // (only one of last_timeout_name_/last_timeout_id_ is meaningful for a given call; the other
  // keeps its previous value). A test may also freely read, clear, or copy these by hand (several
  // do, to fire a stale callback after a rearm) — that is a deliberately supported use, distinct
  // from test_scheduler::run_due()'s pending-timer registry (application.h). Don't mix the two
  // idioms in one test: run_due() has no way to know a hand-fired callback already ran.
  std::function<void()> last_timeout_callback_;
  std::string last_timeout_name_;
  uint32_t last_timeout_id_{0};
  uint32_t last_timeout_ms_{0};

  // Test helpers for verifying set_interval() calls, same rule as above.
  std::function<void()> last_interval_callback_;
  std::string last_interval_name_;
  uint32_t last_interval_ms_{0};

  void mark_failed() { this->failed_ = true; }
  // Real ESPHome overload takes a `const LogString *` (see esphome/core/log.h's LOG_STR());
  // host tests don't need the PROGMEM plumbing, so this just takes the plain string LOG_STR()
  // resolves to here.
  void mark_failed(const char *message) {
    (void) message;
    this->failed_ = true;
  }
  bool is_failed() const { return this->failed_; }
  void set_warn_if_blocking_over(uint32_t ms) { (void) ms; }

 protected:
  bool failed_{false};
  uint32_t warn_if_blocking_over_{0};
};

// Setup priority constants (approximate)
struct setup_priority {
  static constexpr float BEFORE_BOOT = -100.0f;
  static constexpr float HARDWARE = 100.0f;
  static constexpr float DATA = 200.0f;
  static constexpr float AFTER_BOOT = 300.0f;
};

}  // namespace esphome
