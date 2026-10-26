/// @file platform_fan.cpp
/// @brief Experimental fan entity for IO-Homecontrol exterior heating (a heat level, 0-100 %).
/// @ingroup hioc_platforms

#include "platform_fan.h"

#include "entity_helpers.h"
#include "proto_device_types.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.fan";

namespace {

/// Number of speed steps: one per percent of heat.
constexpr int HEAT_STEPS = 100;

}  // namespace

void IOHomeFan::setup() {
  // A known device of another family is a configuration mistake; an unknown type may still resolve
  // at discovery, as on the other platforms.
  if (this->device_type_ != DeviceType::UNKNOWN &&
      device_capability_class(this->device_type_, this->subtype_) != DeviceCapabilityClass::HEATER_LEVEL) {
    ESP_LOGE(TAG,
             "Device %s is declared as %s, which a fan entity cannot drive (it drives exterior heating) — disabling it",
             this->device_id_.c_str(), format_device_kind(this->device_type_, this->subtype_).c_str());
    this->mark_failed();
    return;
  }
  this->register_device_binding_(
      this, DeviceCapabilityClass::HEATER_LEVEL, std::nullopt,
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });
}

fan::FanTraits IOHomeFan::get_traits() {
  // Every exterior heater is sent the main parameter only, but only the on/off kind has no level.
  const bool has_level =
      has_feature(this->device_features_(DeviceCapabilityClass::HEATER_LEVEL), device_feature::LEVELS);
  return fan::FanTraits(/*oscillation=*/false, /*speed=*/has_level, /*direction=*/false, HEAT_STEPS);
}

void IOHomeFan::control(const fan::FanCall &call) {
  const bool has_level = this->get_traits().supports_speed();
  const auto state = call.get_state();
  const auto speed = call.get_speed();

  int heat = 0;
  if (state.has_value() && !*state) {
    heat = 0;
  } else if (speed.has_value() && has_level) {
    heat = std::clamp(*speed, 0, HEAT_STEPS);
  } else if (state.has_value()) {
    // On without a level: the last level the heater was at, else full heat.
    heat = (has_level && this->speed > 0) ? this->speed : HEAT_STEPS;
  } else {
    return;
  }

  // Heat p % is IO position 100 - p, the dimmable light's mapping (detail::round_percent rounds).
  const auto io_position = detail::round_percent(1.0F - (static_cast<float>(heat) / static_cast<float>(HEAT_STEPS)));
  this->parent_->queue_set_fan_position(this->device_id_, io_position);

  // The commanded state stands in until the device's status reply replaces it.
  this->state = heat > 0;
  if (heat > 0 && has_level)
    this->speed = heat;
  this->publish_state();
}

void IOHomeFan::on_device_update_(const std::string &id, const IoDevice &dev) {
  // Publish a stable state only: the shared filter wants a known position and a device at rest
  // (an exterior heater has no motion, so a status reply always counts as settled).
  if (!this->passes_binary_update_filter_(id, dev))
    return;
  const int heat = std::clamp(static_cast<int>(std::lround(100.0F - dev.position)), 0, HEAT_STEPS);
  const bool on = heat > 0;
  const bool has_level = this->get_traits().supports_speed();
  const int speed = (on && has_level) ? heat : this->speed;  // an off heater keeps its last level
  if (this->state == on && this->speed == speed)
    return;
  this->state = on;
  this->speed = speed;
  this->publish_state();
}

void IOHomeFan::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Exterior Heating Fan:");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  this->log_poll_interval_config_(TAG);
  ESP_LOGCONFIG(TAG, "  Status: experimental, unvalidated on hardware");
}

}  // namespace home_io_control
}  // namespace esphome
