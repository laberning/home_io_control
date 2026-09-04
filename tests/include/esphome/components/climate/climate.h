#pragma once

// Minimal host-test stub of ESPHome's climate component. Mirrors only the surface
// platform_climate.{h,cpp} and platform_climate_test.cpp use; the real component is compiled by
// `make firmware-test`.

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "esphome/core/helpers.h"

namespace esphome {
namespace climate {

#define LOG_CLIMATE(prefix, type, obj) ((void) 0)

enum ClimateMode : uint8_t {
  CLIMATE_MODE_OFF = 0,
  CLIMATE_MODE_HEAT_COOL = 1,
  CLIMATE_MODE_COOL = 2,
  CLIMATE_MODE_HEAT = 3,
  CLIMATE_MODE_FAN_ONLY = 4,
  CLIMATE_MODE_DRY = 5,
  CLIMATE_MODE_AUTO = 6,
};

enum ClimatePreset : uint8_t {
  CLIMATE_PRESET_NONE = 0,
  CLIMATE_PRESET_HOME = 1,
  CLIMATE_PRESET_AWAY = 2,
  CLIMATE_PRESET_BOOST = 3,
  CLIMATE_PRESET_COMFORT = 4,
  CLIMATE_PRESET_ECO = 5,
  CLIMATE_PRESET_SLEEP = 6,
  CLIMATE_PRESET_ACTIVITY = 7,
};

class Climate;

class ClimateTraits {
 public:
  void set_supported_modes(std::vector<ClimateMode> modes) { this->supported_modes_ = std::move(modes); }
  void add_supported_mode(ClimateMode mode) { this->supported_modes_.push_back(mode); }
  bool supports_mode(ClimateMode mode) const {
    for (auto m : this->supported_modes_)
      if (m == mode)
        return true;
    return false;
  }
  void add_supported_preset(ClimatePreset preset) { this->supported_presets_.push_back(preset); }
  bool supports_preset(ClimatePreset preset) const {
    for (auto p : this->supported_presets_)
      if (p == preset)
        return true;
    return false;
  }
  // Real ClimateTraits models current-temperature support via feature flags now; this stub only
  // needs the "not supported" default the platform relies on.
  bool get_supports_current_temperature() const { return this->supports_current_temperature_; }
  void set_visual_min_temperature(float value) { this->visual_min_temperature_ = value; }
  float get_visual_min_temperature() const { return this->visual_min_temperature_; }
  void set_visual_max_temperature(float value) { this->visual_max_temperature_ = value; }
  float get_visual_max_temperature() const { return this->visual_max_temperature_; }
  void set_visual_target_temperature_step(float value) { this->visual_target_temperature_step_ = value; }
  float get_visual_target_temperature_step() const { return this->visual_target_temperature_step_; }

 protected:
  std::vector<ClimateMode> supported_modes_;
  std::vector<ClimatePreset> supported_presets_;
  bool supports_current_temperature_{false};
  float visual_min_temperature_{0.0F};
  float visual_max_temperature_{0.0F};
  float visual_target_temperature_step_{0.1F};
};

class ClimateCall {
 public:
  explicit ClimateCall(Climate *parent) : parent_(parent) {}

  ClimateCall &set_mode(ClimateMode mode) {
    this->mode_ = mode;
    return *this;
  }
  ClimateCall &set_target_temperature(float temperature) {
    this->target_temperature_ = temperature;
    return *this;
  }
  ClimateCall &set_preset(ClimatePreset preset) {
    this->preset_ = preset;
    return *this;
  }
  ClimateCall &set_preset(const std::string &custom_preset) {
    this->custom_preset_ = custom_preset;
    return *this;
  }

  const std::optional<ClimateMode> &get_mode() const { return this->mode_; }
  const std::optional<float> &get_target_temperature() const { return this->target_temperature_; }
  const std::optional<ClimatePreset> &get_preset() const { return this->preset_; }
  bool has_custom_preset() const { return this->custom_preset_.has_value(); }
  StringRef get_custom_preset() const { return StringRef(this->custom_preset_.value_or(std::string())); }

  void perform();

 protected:
  Climate *const parent_;
  std::optional<ClimateMode> mode_;
  std::optional<float> target_temperature_;
  std::optional<ClimatePreset> preset_;
  std::optional<std::string> custom_preset_;
};

class Climate {
 public:
  virtual ~Climate() = default;

  ClimateCall make_call() { return ClimateCall(this); }
  void publish_state() { this->publish_count_++; }
  ClimateTraits get_traits() { return this->traits(); }

  void set_supported_custom_presets(std::initializer_list<const char *> presets) {
    this->supported_custom_presets_.assign(presets.begin(), presets.end());
  }
  const std::vector<std::string> &get_supported_custom_presets() const { return this->supported_custom_presets_; }
  const std::string &get_custom_preset() const { return this->custom_preset_; }
  bool has_custom_preset() const { return !this->custom_preset_.empty(); }

  float current_temperature{NAN};
  float target_temperature{NAN};
  ClimateMode mode{CLIMATE_MODE_OFF};
  std::optional<ClimatePreset> preset;

  /// Test-only: number of publish_state() calls.
  int publish_count_{0};

  virtual ClimateTraits traits() = 0;
  virtual void control(const ClimateCall &call) = 0;

 protected:
  bool set_custom_preset_(StringRef preset) {
    this->custom_preset_ = preset.str();
    this->preset.reset();
    return true;
  }
  void clear_custom_preset_() { this->custom_preset_.clear(); }

  std::string custom_preset_;
  std::vector<std::string> supported_custom_presets_;
};

inline void ClimateCall::perform() {
  if (this->parent_ != nullptr)
    this->parent_->control(*this);
}

}  // namespace climate
}  // namespace esphome
