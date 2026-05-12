#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }

  void set_timeout(const char *name, uint32_t timeout, std::function<void()> &&f) {
    last_timeout_name_ = name ? name : "";
    last_timeout_ms_ = timeout;
    last_timeout_callback_ = std::move(f);
  }

  // Test helpers for verifying set_timeout calls
  std::function<void()> last_timeout_callback_;
  std::string last_timeout_name_;
  uint32_t last_timeout_ms_{0};

  void mark_failed() { this->failed_ = true; }
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
