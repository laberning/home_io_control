#pragma once

// Minimal host-test stub of ESPHome's water_heater component. Mirrors only the surface
// platform_water_heater.{h,cpp} and platform_water_heater_test.cpp use; the real component is compiled by
// `make firmware-test`. scripts/check-stub-sync.py pins the signatures against the real header.

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "esphome/core/optional.h"

namespace esphome {
namespace water_heater {

#define LOG_WATER_HEATER(prefix, type, obj) ((void) 0)

enum WaterHeaterMode : uint32_t {
  WATER_HEATER_MODE_OFF = 0,
  WATER_HEATER_MODE_ECO = 1,
  WATER_HEATER_MODE_ELECTRIC = 2,
  WATER_HEATER_MODE_PERFORMANCE = 3,
  WATER_HEATER_MODE_HIGH_DEMAND = 4,
  WATER_HEATER_MODE_HEAT_PUMP = 5,
  WATER_HEATER_MODE_GAS = 6,
};

/// The real header aliases a FiniteSetMask (esphome/core/finite_set_mask.h); a plain bitmask with the same
/// initializer-list construction and count() is all the component uses.
class WaterHeaterModeSet {
 public:
  WaterHeaterModeSet() = default;
  WaterHeaterModeSet(std::initializer_list<WaterHeaterMode> modes) {
    for (const WaterHeaterMode mode : modes)
      this->mask_ |= 1U << static_cast<uint32_t>(mode);
  }
  size_t count(WaterHeaterMode mode) const { return (this->mask_ >> static_cast<uint32_t>(mode)) & 1U; }
  bool empty() const { return this->mask_ == 0; }

 private:
  uint32_t mask_{0};
};
using WaterHeaterModeMask = WaterHeaterModeSet;

enum WaterHeaterFeature : uint32_t {
  WATER_HEATER_SUPPORTS_CURRENT_TEMPERATURE = 1 << 0,
  WATER_HEATER_SUPPORTS_TARGET_TEMPERATURE = 1 << 1,
  WATER_HEATER_SUPPORTS_OPERATION_MODE = 1 << 2,
  WATER_HEATER_SUPPORTS_AWAY_MODE = 1 << 3,
  WATER_HEATER_SUPPORTS_ON_OFF = 1 << 4,
  WATER_HEATER_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE = 1 << 5,
};

/// State flags of a water heater (bitmask), as in the real header.
enum WaterHeaterStateFlag : uint32_t {
  WATER_HEATER_STATE_AWAY = 1 << 0,
  WATER_HEATER_STATE_ON = 1 << 1,
};

class WaterHeater;

class WaterHeaterCall {
 public:
  WaterHeaterCall() : parent_(nullptr) {}
  WaterHeaterCall(WaterHeater *parent) : parent_(parent) {}

  WaterHeaterCall &set_mode(WaterHeaterMode mode) {
    this->mode_ = mode;
    return *this;
  }
  WaterHeaterCall &set_target_temperature(float temperature) {
    this->target_temperature_ = temperature;
    return *this;
  }

  WaterHeaterCall &set_away(bool away) {
    if (away) {
      this->state_ |= WATER_HEATER_STATE_AWAY;
    } else {
      this->state_ &= ~WATER_HEATER_STATE_AWAY;
    }
    this->state_mask_ |= WATER_HEATER_STATE_AWAY;
    return *this;
  }

  void perform();

  const optional<WaterHeaterMode> &get_mode() const { return this->mode_; }
  float get_target_temperature() const { return this->target_temperature_; }
  optional<bool> get_away() const {
    if (this->state_mask_ & WATER_HEATER_STATE_AWAY)
      return (this->state_ & WATER_HEATER_STATE_AWAY) != 0;
    return {};
  }

 protected:
  WaterHeater *parent_;
  optional<WaterHeaterMode> mode_;
  float target_temperature_{NAN};
  uint32_t state_{0};
  uint32_t state_mask_{0};
};

struct WaterHeaterCallInternal : public WaterHeaterCall {
  WaterHeaterCallInternal(WaterHeater *parent) : WaterHeaterCall(parent) {}
};

class WaterHeaterTraits {
 public:
  void add_feature_flags(uint32_t flags) { this->feature_flags_ |= flags; }
  bool has_feature_flags(uint32_t flags) const { return (this->feature_flags_ & flags) == flags; }
  uint32_t get_feature_flags() const { return this->feature_flags_; }

  void set_min_temperature(float min_temperature) { this->min_temperature_ = min_temperature; }
  float get_min_temperature() const { return this->min_temperature_; }
  void set_max_temperature(float max_temperature) { this->max_temperature_ = max_temperature; }
  float get_max_temperature() const { return this->max_temperature_; }
  void set_target_temperature_step(float target_temperature_step) {
    this->target_temperature_step_ = target_temperature_step;
  }
  float get_target_temperature_step() const { return this->target_temperature_step_; }

  void set_supported_modes(WaterHeaterModeMask modes) { this->supported_modes_ = modes; }
  const WaterHeaterModeMask &get_supported_modes() const { return this->supported_modes_; }
  bool supports_mode(WaterHeaterMode mode) const { return this->supported_modes_.count(mode); }

 protected:
  uint32_t feature_flags_{0};
  float min_temperature_{0.0f};
  float max_temperature_{0.0f};
  float target_temperature_step_{0.0f};
  WaterHeaterModeMask supported_modes_;
};

class WaterHeater {
 public:
  virtual ~WaterHeater() = default;

  WaterHeaterMode get_mode() const { return this->mode_; }
  float get_current_temperature() const { return this->current_temperature_; }
  float get_target_temperature() const { return this->target_temperature_; }
  bool is_away() const { return (this->state_ & WATER_HEATER_STATE_AWAY) != 0; }

  void set_current_temperature(float current_temperature) { this->current_temperature_ = current_temperature; }

  virtual void publish_state() { this->publish_count_++; }
  virtual WaterHeaterTraits get_traits() { return this->traits(); }
  virtual WaterHeaterCallInternal make_call() = 0;
  virtual void control(const WaterHeaterCall &call) = 0;

  /// Test-only: number of publish_state() calls.
  int publish_count_{0};

 protected:
  virtual WaterHeaterTraits traits() = 0;

  void set_mode_(WaterHeaterMode mode) { this->mode_ = mode; }
  void set_target_temperature_(float target_temperature) { this->target_temperature_ = target_temperature; }
  void set_state_flag_(uint32_t flag, bool value) {
    if (value) {
      this->state_ |= flag;
    } else {
      this->state_ &= ~flag;
    }
  }

  WaterHeaterMode mode_{WATER_HEATER_MODE_OFF};
  float current_temperature_{NAN};
  float target_temperature_{NAN};
  uint32_t state_{0};
};

// The real perform() validates against the traits first (an unsupported mode is dropped, a target is clamped
// into the traits' range), then calls control(). The stub does the same two checks, so a test that goes
// through perform() sees what the firmware would hand control().
inline void WaterHeaterCall::perform() {
  if (this->parent_ == nullptr)
    return;
  const WaterHeaterTraits traits = this->parent_->get_traits();
  if (this->mode_.has_value() && !traits.supports_mode(*this->mode_))
    this->mode_.reset();
  if (!std::isnan(this->target_temperature_)) {
    if (this->target_temperature_ < traits.get_min_temperature())
      this->target_temperature_ = traits.get_min_temperature();
    if (this->target_temperature_ > traits.get_max_temperature())
      this->target_temperature_ = traits.get_max_temperature();
  }
  this->parent_->control(*this);
}

}  // namespace water_heater
}  // namespace esphome
