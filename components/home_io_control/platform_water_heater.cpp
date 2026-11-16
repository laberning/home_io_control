/// @file platform_water_heater.cpp
/// @brief Water heater entity for a domestic hot water heater, and its opt-in companion sensors.
/// @ingroup hioc_platforms

#include "platform_water_heater.h"

#include "entity_helpers.h"
#include "proto_device_kinds.h"
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
/// The two modes of an electric storage water heater: it has no heat pump, so its automatic mode is ECO
/// (it learns the household's habits and heats only what they need) and its manual mode is ELECTRIC.
constexpr WaterHeaterModeMapping ELECTRIC_WATER_HEATER_MODE_MAP[] = {
    {DhwMode::AUTO, water_heater::WATER_HEATER_MODE_ECO},
    {DhwMode::MANUAL_ECO_OFF, water_heater::WATER_HEATER_MODE_ELECTRIC},
};

/// The mode table of a model, as a range.
struct WaterHeaterModeMap {
  const WaterHeaterModeMapping *begin_;
  const WaterHeaterModeMapping *end_;
  [[nodiscard]] const WaterHeaterModeMapping *begin() const { return this->begin_; }
  [[nodiscard]] const WaterHeaterModeMapping *end() const { return this->end_; }
};

WaterHeaterModeMap mode_map_for(DeviceModel model) {
  if (dhw_state_in_registers(model))
    return {std::begin(ELECTRIC_WATER_HEATER_MODE_MAP), std::end(ELECTRIC_WATER_HEATER_MODE_MAP)};
  return {std::begin(WATER_HEATER_MODE_MAP), std::end(WATER_HEATER_MODE_MAP)};
}

/// The target range a model's card offers.
float target_min_for(DeviceModel model) {
  return dhw_state_in_registers(model) ? DHW_ELECTRIC_TARGET_MIN_C : DHW_TARGET_MIN_C;
}
float target_max_for(DeviceModel model) {
  return dhw_state_in_registers(model) ? DHW_ELECTRIC_TARGET_MAX_C : DHW_TARGET_MAX_C;
}

/// @brief UI target step: every target the heater has shown is a whole degree (issue #198). The main
/// parameter resolves far finer.
constexpr float WATER_HEATER_TARGET_STEP_C = 1.0F;

/// @brief Wh per kWh, for the energy sensor.
constexpr float WH_PER_KWH = 1000.0F;

}  // namespace

water_heater::WaterHeaterMode water_heater_mode_for(DhwMode mode, DeviceModel model) {
  const WaterHeaterModeMap map = mode_map_for(model);
  for (const auto &mapping : map) {
    if (mapping.dhw == mode)
      return mapping.mode;
  }
  // A mode outside the model's table shows as its automatic mode, the table's first row.
  return map.begin()->mode;
}

bool dhw_mode_for(water_heater::WaterHeaterMode mode, DeviceModel model, DhwMode &out) {
  for (const auto &mapping : mode_map_for(model)) {
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
  if (this->reject_undrivable_kind_(
          TAG,
          has_feature(device_features(this->device_type_, this->subtype_, this->model_), device_feature::WATER_HEATER),
          "which is not a water heater (io_device_type: domestic_hot_water)")) {
    this->mark_failed();
    return;
  }

  // The initial poll reads the heater (HubDevicePort::queue_request_device_status()).
  this->register_device_binding_(this, DeviceCapabilityClass::WATER_HEATER,
                                 [this](const IoDevice &dev) { this->on_device_update_(dev); });
}

water_heater::WaterHeaterTraits IOHomeWaterHeater::traits() {
  water_heater::WaterHeaterTraits traits;
  // Both temperatures are filled only from a polled reply: until the first poll lands the card shows them
  // empty, which is the honest state.
  traits.add_feature_flags(
      water_heater::WATER_HEATER_SUPPORTS_CURRENT_TEMPERATURE | water_heater::WATER_HEATER_SUPPORTS_TARGET_TEMPERATURE |
      water_heater::WATER_HEATER_SUPPORTS_OPERATION_MODE | water_heater::WATER_HEATER_SUPPORTS_AWAY_MODE);
  traits.set_min_temperature(target_min_for(this->model_));
  traits.set_max_temperature(target_max_for(this->model_));
  traits.set_target_temperature_step(WATER_HEATER_TARGET_STEP_C);
  if (dhw_state_in_registers(this->model_)) {
    traits.set_supported_modes({water_heater::WATER_HEATER_MODE_ECO, water_heater::WATER_HEATER_MODE_ELECTRIC});
  } else {
    traits.set_supported_modes({water_heater::WATER_HEATER_MODE_HEAT_PUMP, water_heater::WATER_HEATER_MODE_ECO,
                                water_heater::WATER_HEATER_MODE_PERFORMANCE});
  }
  return traits;
}

void IOHomeWaterHeater::control(const water_heater::WaterHeaterCall &call) {
  if (this->heating_port_ == nullptr)
    return;
  bool changed = false;

  if (call.get_mode().has_value()) {
    DhwMode mode = DhwMode::AUTO;
    if (!dhw_mode_for(*call.get_mode(), this->model_, mode)) {
      // ESPHome drops a mode the traits do not list before control(); this guards a direct caller.
      ESP_LOGW(TAG, "Device %s: mode %" PRIu32 " is not one this heater offers", this->device_id_.c_str(),
               static_cast<uint32_t>(*call.get_mode()));
    } else if (this->heating_port_->set_water_heater_mode(this->device_id_, mode) && !this->observed_) {
      // The read-back publishes through on_device_update_(); only before any reading the command is shown.
      this->set_mode_(*call.get_mode());
      changed = true;
    }
  }

  // The heater reports away from FP16 in the read-back, which publishes it; the command is never shown.
  if (call.get_away().has_value())
    this->heating_port_->set_water_heater_away(this->device_id_, *call.get_away());

  const float target = call.get_target_temperature();
  // Away is judged on the call's request (a call that starts away sends no target), or else on what the
  // heater reports. The mode is judged on what the heater holds after the mode write above (its read-back
  // has already been folded in), not on the mode the call asked for: a refused mode write leaves the heater
  // where it was.
  const bool away = call.get_away().value_or(this->is_away());
  if (!std::isnan(target) && away) {
    ESP_LOGW(TAG, "Device %s: target ignored while away, the heater holds no target then", this->device_id_.c_str());
  } else if (!std::isnan(target) && this->observed_ && this->mode_ == water_heater::WATER_HEATER_MODE_HEAT_PUMP) {
    ESP_LOGW(TAG, "Device %s: target ignored in automatic mode, the heater chooses its own", this->device_id_.c_str());
  } else if (!std::isnan(target) && this->heating_port_->set_water_heater_target(this->device_id_, target) &&
             !this->observed_) {
    // The settle poll reads the target back from the status reply.
    this->set_target_temperature_(target);
    changed = true;
  }

  if (changed)
    this->publish_state();
}

bool IOHomeWaterHeater::sync_from_readings_(const DhwReadings &readings) {
  bool changed = false;
  if (detail::reading_changed(this->current_temperature_, readings.water_c)) {
    this->set_current_temperature(readings.water_c);
    changed = true;
  }
  // A target that is not a temperature (an alias the heater has not been seen sending) shows as unknown, and
  // so does the target in automatic mode (the heater chooses its own) and while away (the heater reports a
  // main parameter of 0 then, which would read as 20 degrees but is no target).
  // An electric storage water heater reports the temperature it heats to in every state (its automatic
  // choice and the frost protection of its absence included), so its target is always shown.
  const bool automatic = readings.mode == static_cast<int16_t>(DhwMode::AUTO);
  const bool hidden = !dhw_state_in_registers(this->model_) && (automatic || readings.away == 1);
  const float target = hidden ? NAN : readings.target_c;
  if (detail::reading_changed(this->target_temperature_, target)) {
    this->set_target_temperature_(target);
    changed = true;
  }
  if (readings.mode >= 0) {
    const water_heater::WaterHeaterMode mode =
        water_heater_mode_for(static_cast<DhwMode>(readings.mode), this->model_);
    if (mode != this->mode_) {
      this->set_mode_(mode);
      changed = true;
    }
  }
  if (readings.away >= 0 && (readings.away != 0) != this->is_away()) {
    this->set_state_flag_(water_heater::WATER_HEATER_STATE_AWAY, readings.away != 0);
    changed = true;
  }
  this->observed_ =
      this->observed_ || !std::isnan(readings.water_c) || !std::isnan(readings.target_c) || readings.mode >= 0;
  return changed;
}

void IOHomeWaterHeater::on_device_update_(const IoDevice &dev) {
  if (this->sync_from_readings_(dev.dhw))
    this->publish_state();
}

void IOHomeWaterHeater::dump_config() {
  LOG_WATER_HEATER("", "IO-Homecontrol Water Heater", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Target temperature range: %.0f-%.0f C", static_cast<double>(target_min_for(this->model_)),
                static_cast<double>(target_max_for(this->model_)));
  this->log_poll_interval_config_(TAG, heater_poll_interval_ms);
  if (dhw_state_in_registers(this->model_)) {
    ESP_LOGCONFIG(TAG, "  Status: electric storage water heater; readings, mode, boost and away confirmed on "
                       "hardware; a target is shown but not sent");
  } else {
    ESP_LOGCONFIG(TAG, "  Status: readings, target and mode confirmed on hardware; away mode experimental");
  }
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
  }
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Water Heater %s:", reading);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeDhwBoostSwitch::setup() {
  // No restore: the state comes only from the heater, and a reboot must not start or end a boost.
  this->register_companion_binding_([this](const IoDevice &dev) {
    // Shown only from the heater's FP16 report; a switch with no reading yet keeps its unknown state.
    if (dev.dhw.boost >= 0)
      this->publish_state(dev.dhw.boost != 0);
  });
}

void IOHomeDhwBoostSwitch::write_state(bool state) {
  if (this->heating_port_ == nullptr)
    return;
  // The status read inside HubHeatingPort::set_water_heater_boost() updates IoDevice::dhw, and the binding above
  // publishes it.
  if (!this->heating_port_->set_water_heater_boost(this->device_id_, state)) {
    ESP_LOGW(TAG, "Device %s: boost %s was not acknowledged", this->device_id_.c_str(), state ? "on" : "off");
  }
}

void IOHomeDhwBoostSwitch::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Water Heater Boost:");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Status: experimental (a boost started here writes duration 1; the state is the heater's FP16)");
}

}  // namespace home_io_control
}  // namespace esphome
