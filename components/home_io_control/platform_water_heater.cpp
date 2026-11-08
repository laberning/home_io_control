/// @file platform_water_heater.cpp
/// @brief Water heater entity for a domestic hot water heater, and its opt-in companion sensors.
/// @ingroup hioc_platforms

#include "platform_water_heater.h"

#include "proto_device_types.h"
#include "status_poll_policy.h"

#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.water_heater";

namespace {

/// @brief Heater mode <-> ESPHome mode. The single place this correspondence is defined (ADR 0068).
struct WaterHeaterModeMapping {
  DhwMode dhw;
  water_heater::WaterHeaterMode mode;
};
constexpr WaterHeaterModeMapping WATER_HEATER_MODE_MAP[] = {
    {DhwMode::AUTO, water_heater::WATER_HEATER_MODE_HEAT_PUMP},
    {DhwMode::MANUAL_ECO_ON, water_heater::WATER_HEATER_MODE_ECO},
    {DhwMode::MANUAL_ECO_OFF, water_heater::WATER_HEATER_MODE_PERFORMANCE},
};

/// @brief UI target step. The main parameter resolves far finer; 0.5 C is a card choice.
constexpr float WATER_HEATER_TARGET_STEP_C = 0.5F;

/// @brief Wh per kWh, for the energy sensor.
constexpr float WH_PER_KWH = 1000.0F;

}  // namespace

water_heater::WaterHeaterMode water_heater_mode_for(DhwMode mode) {
  for (const auto &mapping : WATER_HEATER_MODE_MAP) {
    if (mapping.dhw == mode)
      return mapping.mode;
  }
  return water_heater::WATER_HEATER_MODE_HEAT_PUMP;
}

bool dhw_mode_for(water_heater::WaterHeaterMode mode, DhwMode &out) {
  for (const auto &mapping : WATER_HEATER_MODE_MAP) {
    if (mapping.mode == mode) {
      out = mapping.dhw;
      return true;
    }
  }
  return false;
}

// === IOHomeWaterHeater ===

void IOHomeWaterHeater::setup() {
  // A known device that is no water heater is a configuration mistake the codegen already refuses; fail
  // loudly rather than let every command be rejected downstream, and before the binding registers it.
  if (this->device_type_ != DeviceType::UNKNOWN &&
      !has_feature(device_features(this->device_type_, this->subtype_, this->model_), device_feature::WATER_HEATER)) {
    ESP_LOGE(TAG,
             "Device %s is declared as %s, which is not a water heater (io_device_type: domestic_hot_water) — "
             "disabling it",
             this->device_id_.c_str(), format_device_kind(this->device_type_, this->subtype_).c_str());
    this->mark_failed();
    return;
  }

  // The initial poll reads the heater (IOHomeControlComponent::request_device_status()).
  this->register_device_binding_(
      this, DeviceCapabilityClass::WATER_HEATER, std::nullopt,
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });
}

water_heater::WaterHeaterTraits IOHomeWaterHeater::traits() {
  water_heater::WaterHeaterTraits traits;
  // Both temperatures are filled only from a polled reply: until the first poll lands the card shows them
  // empty, which is the honest state.
  traits.add_feature_flags(water_heater::WATER_HEATER_SUPPORTS_CURRENT_TEMPERATURE |
                           water_heater::WATER_HEATER_SUPPORTS_TARGET_TEMPERATURE |
                           water_heater::WATER_HEATER_SUPPORTS_OPERATION_MODE);
  traits.set_min_temperature(DHW_TARGET_MIN_C);
  traits.set_max_temperature(DHW_TARGET_MAX_C);
  traits.set_target_temperature_step(WATER_HEATER_TARGET_STEP_C);
  traits.set_supported_modes({water_heater::WATER_HEATER_MODE_HEAT_PUMP, water_heater::WATER_HEATER_MODE_ECO,
                              water_heater::WATER_HEATER_MODE_PERFORMANCE});
  return traits;
}

void IOHomeWaterHeater::control(const water_heater::WaterHeaterCall &call) {
  if (this->parent_ == nullptr)
    return;
  bool changed = false;

  if (call.get_mode().has_value()) {
    DhwMode mode = DhwMode::AUTO;
    if (!dhw_mode_for(*call.get_mode(), mode)) {
      // ESPHome drops a mode the traits do not list before control(); this guards a direct caller.
      ESP_LOGW(TAG, "Device %s: mode %" PRIu32 " is not one this heater offers", this->device_id_.c_str(),
               static_cast<uint32_t>(*call.get_mode()));
    } else if (this->parent_->set_water_heater_mode(this->device_id_, mode) && !this->observed_) {
      // The read-back publishes through on_device_update_(); only before any reading the command is shown.
      this->set_mode_(*call.get_mode());
      changed = true;
    }
  }

  const float target = call.get_target_temperature();
  if (!std::isnan(target) && this->parent_->set_water_heater_target(this->device_id_, target) && !this->observed_) {
    // The settle poll reads the target back from the status reply.
    this->set_target_temperature_(target);
    changed = true;
  }

  if (changed)
    this->publish_state();
}

bool IOHomeWaterHeater::sync_from_readings_(const DhwReadings &readings) {
  bool changed = false;
  // NaN means "never read": two NaNs are the same state, not a change.
  const auto differs = [](float a, float b) { return !(std::isnan(a) && std::isnan(b)) && a != b; };

  if (differs(this->current_temperature_, readings.water_c)) {
    this->set_current_temperature(readings.water_c);
    changed = true;
  }
  // A target that is not a temperature (an alias the heater has not been seen sending) shows as unknown.
  if (differs(this->target_temperature_, readings.target_c)) {
    this->set_target_temperature_(readings.target_c);
    changed = true;
  }
  if (readings.mode >= 0) {
    const water_heater::WaterHeaterMode mode = water_heater_mode_for(static_cast<DhwMode>(readings.mode));
    if (mode != this->mode_) {
      this->set_mode_(mode);
      changed = true;
    }
  }
  this->observed_ =
      this->observed_ || !std::isnan(readings.water_c) || !std::isnan(readings.target_c) || readings.mode >= 0;
  return changed;
}

void IOHomeWaterHeater::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;
  if (this->sync_from_readings_(dev.dhw))
    this->publish_state();
}

void IOHomeWaterHeater::dump_config() {
  LOG_WATER_HEATER("", "IO-Homecontrol Water Heater", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Target temperature range: %.0f-%.0f C", static_cast<double>(DHW_TARGET_MIN_C),
                static_cast<double>(DHW_TARGET_MAX_C));
  ESP_LOGCONFIG(
      TAG, "  Status Poll Interval: %" PRIu32 " ms",
      this->status_poll_interval_ms_ != 0 ? this->status_poll_interval_ms_ : HEATING_POLL_DEFAULT_INTERVAL_MS);
  ESP_LOGCONFIG(TAG, "  Status: confirmed on hardware (readings, target, mode)");
}

// === Companions ===

void IOHomeDhwEnergySensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (dev.dhw.energy_wh >= 0)
      this->publish_state(static_cast<float>(dev.dhw.energy_wh) / WH_PER_KWH);
  });
}

void IOHomeDhwEnergySensor::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Water Heater Energy:");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeDhwBinarySensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    int8_t value = -1;
    switch (this->reading_) {
      case Reading::HEAT_PUMP:
        value = dev.dhw.heat_pump;
        break;
      case Reading::ELECTRIC_BACKUP:
        value = dev.dhw.element;
        break;
      case Reading::BOOST:
        value = dev.dhw.boost;
        break;
    }
    // "Off" must mean the heater said so: nothing read means no state.
    if (value < 0) {
      this->invalidate_state();
      return;
    }
    this->publish_state(value != 0);
  });
}

void IOHomeDhwBinarySensor::dump_config() {
  const char *reading = "heat pump running";
  switch (this->reading_) {
    case Reading::HEAT_PUMP:
      break;
    case Reading::ELECTRIC_BACKUP:
      reading = "electric backup running";
      break;
    case Reading::BOOST:
      reading = "boost";
      break;
  }
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Water Heater %s:", reading);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
