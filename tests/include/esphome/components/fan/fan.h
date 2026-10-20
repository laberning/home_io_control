#pragma once

// Minimal host-test stub of ESPHome's fan component. Mirrors only the surface platform_fan.{h,cpp}
// and platform_fan_test.cpp use; the real component is compiled by `make firmware-test`.

#include <algorithm>
#include <cstdint>
#include <optional>

namespace esphome {
namespace fan {

#define LOG_FAN(prefix, type, obj) ((void) 0)

class Fan;

class FanTraits {
 public:
  FanTraits() = default;
  FanTraits(bool oscillation, bool speed, bool direction, int speed_count)
      : oscillation_(oscillation), speed_(speed), direction_(direction), speed_count_(speed_count) {}

  bool supports_oscillation() const { return this->oscillation_; }
  bool supports_speed() const { return this->speed_; }
  void set_speed(bool speed) { this->speed_ = speed; }
  int supported_speed_count() const { return this->speed_count_; }
  void set_supported_speed_count(int speed_count) { this->speed_count_ = speed_count; }
  bool supports_direction() const { return this->direction_; }

 protected:
  bool oscillation_{false};
  bool speed_{false};
  bool direction_{false};
  int speed_count_{};
};

class FanCall {
 public:
  explicit FanCall(Fan &parent) : parent_(parent) {}

  FanCall &set_state(bool binary_state) {
    this->binary_state_ = binary_state;
    return *this;
  }
  std::optional<bool> get_state() const { return this->binary_state_; }
  FanCall &set_speed(int speed) {
    this->speed_ = speed;
    return *this;
  }
  std::optional<int> get_speed() const { return this->speed_; }

  void perform();

 protected:
  Fan &parent_;
  std::optional<bool> binary_state_;
  std::optional<int> speed_;
};

class Fan {
 public:
  virtual ~Fan() = default;

  /// The current on/off state of the fan.
  bool state{false};
  /// The current fan speed level.
  int speed{0};

  FanCall make_call() { return FanCall(*this); }
  void publish_state() { this->publish_count_++; }
  virtual FanTraits get_traits() = 0;

  /// Test-only: number of publish_state() calls.
  int publish_count_{0};

  /// Test-only: run control() the way FanCall::perform() does, including the speed clamp and the
  /// "turning on with no speed known sets full speed" rule of the real FanCall::validate_().
  void perform_call(FanCall &call);

 protected:
  friend FanCall;
  virtual void control(const FanCall &call) = 0;
};

inline void FanCall::perform() { this->parent_.perform_call(*this); }

inline void Fan::perform_call(FanCall &call) {
  const FanTraits traits = this->get_traits();
  if (call.get_speed().has_value())
    call.set_speed(std::clamp(*call.get_speed(), 1, traits.supported_speed_count()));
  if (!this->state && call.get_state().has_value() && *call.get_state() && traits.supports_speed() &&
      this->speed == 0 && !call.get_speed().has_value())
    call.set_speed(traits.supported_speed_count());
  this->control(call);
}

}  // namespace fan
}  // namespace esphome
