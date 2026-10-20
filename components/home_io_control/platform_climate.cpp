/// @file platform_climate.cpp
/// @brief Experimental climate entity for IO-Homecontrol heating devices (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_platforms

#include "platform_climate.h"

#include "proto_device_types.h"
#include "status_poll_policy.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <string>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.climate";

namespace {

/// @brief HA HVAC mode <-> IO heating mode. The single place this correspondence is defined.
struct ClimateModeMapping {
  climate::ClimateMode climate_mode;
  HeatingMode heating_mode;
};
constexpr ClimateModeMapping CLIMATE_MODE_MAP[] = {
    {climate::CLIMATE_MODE_OFF, HeatingMode::OFF},
    {climate::CLIMATE_MODE_HEAT, HeatingMode::MANUAL},
    {climate::CLIMATE_MODE_AUTO, HeatingMode::AUTO},
};

/// @brief HA preset <-> IO presence value (SET_PRESENCE: 1 = present/home, 0 = absent/away).
struct ClimatePresetMapping {
  climate::ClimatePreset preset;
  float presence_value;
};
constexpr ClimatePresetMapping CLIMATE_PRESET_MAP[] = {
    {climate::CLIMATE_PRESET_HOME, 1.0F},
    {climate::CLIMATE_PRESET_AWAY, 0.0F},
};

/// @brief How a mode read from the heater shows on the card. The device's own schedule has no HVAC
/// mode, so program mode is auto with the "Program" custom preset naming it; the external mode (the
/// setpoint comes from another controller) is shown as the nearest HVAC mode, heat.
struct ObservedModeMapping {
  HeatingMode heating_mode;
  climate::ClimateMode climate_mode;
  bool program;  ///< Show the "Program" custom preset.
};
constexpr ObservedModeMapping OBSERVED_MODE_MAP[] = {
    {HeatingMode::AUTO, climate::CLIMATE_MODE_AUTO, false}, {HeatingMode::MANUAL, climate::CLIMATE_MODE_HEAT, false},
    {HeatingMode::PROG, climate::CLIMATE_MODE_AUTO, true},  {HeatingMode::EXTERNAL, climate::CLIMATE_MODE_HEAT, false},
    {HeatingMode::OFF, climate::CLIMATE_MODE_OFF, false},
};

/// @brief Custom preset name standing in for HeatingMode::PROG (there is no ClimateMode for a
/// device-side schedule).
constexpr const char *PROGRAM_PRESET_NAME = "Program";

/// @brief UI target-temperature step. The wire carries 0.1 C (encode_heating_write() encodes a
/// 16-bit tenths-of-a-degree setpoint); 0.5 C is a UI-only choice for a sane thermostat card.
constexpr float CLIMATE_TARGET_TEMP_STEP_C = 0.5F;

}  // namespace

void IOHomeClimate::setup() {
  // Device gating, the single predicate. A known device this entity cannot drive (a non-climate
  // type, or a heater without the adjustable-setpoint subtype) is a configuration mistake, so fail
  // it loudly rather than let every command be rejected downstream. An UNKNOWN type is left to pass through: discovery
  // may still resolve it to a climate type, exactly as the other platforms tolerate unknown bindings. This runs BEFORE
  // register_device_binding_() so a misconfigured binding never injects a device into the hub registry and never leaves
  // a live callback on a failed entity.
  if (this->device_type_ != DeviceType::UNKNOWN &&
      !device_supports_climate_control(this->device_type_, this->subtype_)) {
    ESP_LOGE(TAG,
             "Device %s is declared as %s, which this climate entity cannot drive yet (it drives an electrical heater "
             "with an adjustable setpoint: io_device_type: electrical_heater, io_subtype: 1) — disabling it",
             this->device_id_.c_str(), format_device_kind(this->device_type_, this->subtype_).c_str());
    this->mark_failed();
    return;
  }

  // The initial poll reads the heater's registers (IOHomeControlComponent::poll_heating_device_()), so
  // the card shows measured values from the first minute instead of nothing.
  this->register_device_binding_(this, /*inverted=*/false, [this](const std::string &id, const IoDevice &dev) {
    this->on_device_update_(id, dev);
  });

  // "Program" is a custom preset (registered on the entity, referenced by the traits) because IO's
  // prog mode has no ClimateMode equivalent.
  this->set_supported_custom_presets({PROGRAM_PRESET_NAME});
}

climate::ClimateTraits IOHomeClimate::traits() {
  climate::ClimateTraits traits;
  // Both are filled only from a polled reply (sync_from_readings_()): a card that advertises them
  // shows an empty value until the first poll lands, which is the honest state.
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  for (const auto &mapping : CLIMATE_MODE_MAP)
    traits.add_supported_mode(mapping.climate_mode);
  for (const auto &mapping : CLIMATE_PRESET_MAP)
    traits.add_supported_preset(mapping.preset);
  // HEATING_TEMP_MIN_C up to the heater's own maximum setpoint once read, never past what
  // encode_heating_write() accepts (HEATING_TEMP_MAX_C).
  traits.set_visual_min_temperature(HEATING_TEMP_MIN_C);
  traits.set_visual_max_temperature(this->max_temperature_c_);
  traits.set_visual_target_temperature_step(CLIMATE_TARGET_TEMP_STEP_C);
  return traits;
}

void IOHomeClimate::control(const climate::ClimateCall &call) {
  // Each field is independent: a failed send for one leaves that field's state in place and does not
  // block the others. Every helper runs; publish once if any field advanced without a read-back.
  const bool mode_changed = this->apply_mode_(call);
  const bool custom_preset_changed = this->apply_custom_preset_(call);
  const bool preset_changed = this->apply_preset_(call);
  const bool temperature_changed = this->apply_target_temperature_(call);

  // A read-back publishes through on_device_update_(); this publishes what a write advanced when no
  // read-back has confirmed anything yet, and the presence preset, which has no register to read.
  if (mode_changed || custom_preset_changed || preset_changed || temperature_changed)
    this->publish_state();
}

void IOHomeClimate::confirm_write_(HeatingFunction fn) {
  // A failed read is already logged by the hub, and the poll will find the register again; the
  // entity keeps whatever it showed.
  this->parent_->read_heating_register(this->device_id_, fn, nullptr, EXCHANGE_RETRY_COUNT);
}

bool IOHomeClimate::apply_mode_(const climate::ClimateCall &call) {
  const auto &mode_opt = call.get_mode();
  if (!mode_opt.has_value())
    return false;
  const climate::ClimateMode requested = *mode_opt;
  for (const auto &mapping : CLIMATE_MODE_MAP) {
    if (mapping.climate_mode != requested)
      continue;
    if (!this->parent_->send_heating_command(this->device_id_, HeatingFunction::SET_MODE,
                                             static_cast<float>(mapping.heating_mode)))
      return false;
    // Until a read has succeeded there is nothing observed to show: the commanded mode stands in.
    const bool commanded = !this->observed_;
    if (commanded) {
      this->mode = requested;
      this->clear_custom_preset_();  // a real HVAC mode supersedes the "Program" custom preset
    }
    this->confirm_write_(HeatingFunction::SET_MODE);
    return commanded && !this->observed_;
  }
  return false;
}

bool IOHomeClimate::apply_custom_preset_(const climate::ClimateCall &call) {
  if (!call.has_custom_preset())
    return false;
  const std::string custom = std::string(call.get_custom_preset());
  if (custom != PROGRAM_PRESET_NAME)
    return false;
  if (!this->parent_->send_heating_command(this->device_id_, HeatingFunction::SET_MODE,
                                           static_cast<float>(HeatingMode::PROG)))
    return false;
  const bool commanded = !this->observed_;
  if (commanded)
    this->set_custom_preset_(call.get_custom_preset());
  this->confirm_write_(HeatingFunction::SET_MODE);
  return commanded && !this->observed_;
}

bool IOHomeClimate::apply_preset_(const climate::ClimateCall &call) {
  const auto &preset_opt = call.get_preset();
  if (!preset_opt.has_value())
    return false;
  const climate::ClimatePreset requested = *preset_opt;
  for (const auto &mapping : CLIMATE_PRESET_MAP) {
    if (mapping.preset != requested)
      continue;
    if (!this->parent_->send_heating_command(this->device_id_, HeatingFunction::SET_PRESENCE, mapping.presence_value))
      return false;
    // The presence register is write-only, so this stays "last commanded" for good.
    this->preset = requested;
    this->clear_custom_preset_();  // a HOME/AWAY preset supersedes the "Program" custom preset
    return true;
  }
  return false;
}

bool IOHomeClimate::apply_target_temperature_(const climate::ClimateCall &call) {
  const auto &target_opt = call.get_target_temperature();
  if (!target_opt.has_value())
    return false;
  const float target = *target_opt;
  if (!this->parent_->send_heating_command(this->device_id_, HeatingFunction::SET_TEMPERATURE, target))
    return false;
  const bool commanded = !this->observed_;
  if (commanded)
    this->target_temperature = target;
  this->confirm_write_(HeatingFunction::SET_TEMPERATURE);
  return commanded && !this->observed_;
}

bool IOHomeClimate::sync_from_readings_(const HeatingReadings &readings) {
  bool changed = false;
  bool seen = false;
  // NaN means "never read" (or a sensor fault): two NaNs are the same state, not a change.
  const auto differs = [](float a, float b) { return !(std::isnan(a) && std::isnan(b)) && a != b; };

  if (differs(this->current_temperature, readings.room_temperature_c)) {
    this->current_temperature = readings.room_temperature_c;
    changed = true;
  }
  seen = seen || !std::isnan(readings.room_temperature_c);

  if (!std::isnan(readings.setpoint_c)) {
    seen = true;
    if (differs(this->target_temperature, readings.setpoint_c)) {
      this->target_temperature = readings.setpoint_c;
      changed = true;
    }
  }

  if (readings.mode >= 0) {
    seen = true;
    changed = this->apply_observed_mode_(static_cast<uint8_t>(readings.mode)) || changed;
  }

  // The action follows the mode: an off heater is not "idle". Until the regulator has been read only
  // the off case is known.
  climate::ClimateAction action = this->action;
  if (this->mode == climate::CLIMATE_MODE_OFF && readings.mode >= 0) {
    action = climate::CLIMATE_ACTION_OFF;
  } else if (readings.regulator >= 0) {
    seen = true;
    action = readings.regulator == static_cast<int16_t>(HeatingRegulatorState::HEATING)
                 ? climate::CLIMATE_ACTION_HEATING
                 : climate::CLIMATE_ACTION_IDLE;
  }
  if (action != this->action) {
    this->action = action;
    changed = true;
  }

  // The heater's own ceiling, but never past what a write can encode.
  if (!std::isnan(readings.max_setpoint_c) && readings.max_setpoint_c > HEATING_TEMP_MIN_C) {
    seen = true;
    const float max_c = std::min(readings.max_setpoint_c, HEATING_TEMP_MAX_C);
    if (max_c != this->max_temperature_c_) {
      this->max_temperature_c_ = max_c;
      changed = true;
    }
  }

  this->observed_ = this->observed_ || seen;
  return changed;
}

bool IOHomeClimate::apply_observed_mode_(uint8_t heating_mode) {
  const ObservedModeMapping *observed = nullptr;
  for (const auto &mapping : OBSERVED_MODE_MAP) {
    if (static_cast<uint8_t>(mapping.heating_mode) == heating_mode)
      observed = &mapping;
  }
  if (observed == nullptr)
    return false;  // unknown value: leave the mode as it was
  if (observed->heating_mode == HeatingMode::EXTERNAL) {
    // This component never writes the external mode, so seeing it means another controller changed
    // the heater.
    ESP_LOGI(TAG, "Device %s reports mode 'external'; shown as heat", this->device_id_.c_str());
  }

  bool changed = observed->climate_mode != this->mode;
  this->mode = observed->climate_mode;
  const bool program_shown = this->has_custom_preset() && this->get_custom_preset() == PROGRAM_PRESET_NAME;
  if (observed->program && !program_shown) {
    this->set_custom_preset_(PROGRAM_PRESET_NAME);
    changed = true;
  } else if (!observed->program && program_shown) {
    this->clear_custom_preset_();
    changed = true;
  }
  return changed;
}

void IOHomeClimate::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;
  if (this->sync_from_readings_(dev.heating))
    this->publish_state();
}

void IOHomeClimate::dump_config() {
  LOG_CLIMATE("", "IO-Homecontrol Climate", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Target temperature range: %.1f-%.1f C (maximum from the heater once read)", HEATING_TEMP_MIN_C,
                this->max_temperature_c_);
  ESP_LOGCONFIG(
      TAG, "  Status Poll Interval: %" PRIu32 " ms",
      this->status_poll_interval_ms_ != 0 ? this->status_poll_interval_ms_ : HEATING_POLL_DEFAULT_INTERVAL_MS);
  ESP_LOGCONFIG(TAG, "  Status: experimental, unvalidated on hardware");
}

}  // namespace home_io_control
}  // namespace esphome
