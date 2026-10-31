/// @file platform_climate.cpp
/// @brief Experimental climate entity for IO-Homecontrol heaters, driven by a heating profile.
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

/// @brief Profile HVAC mode <-> ESPHome climate mode. The single place this correspondence is defined.
struct ClimateModeMapping {
  HeatingHvacMode hvac;
  climate::ClimateMode climate_mode;
};
constexpr ClimateModeMapping CLIMATE_MODE_MAP[] = {
    {HeatingHvacMode::OFF, climate::CLIMATE_MODE_OFF},
    {HeatingHvacMode::HEAT, climate::CLIMATE_MODE_HEAT},
    {HeatingHvacMode::AUTO, climate::CLIMATE_MODE_AUTO},
};

/// @brief Profile standard preset <-> ESPHome preset. Custom presets are named by the profile.
struct ClimatePresetMapping {
  HeatingPresetId id;
  climate::ClimatePreset preset;
};
constexpr ClimatePresetMapping CLIMATE_PRESET_MAP[] = {
    {HeatingPresetId::AWAY, climate::CLIMATE_PRESET_AWAY},
    {HeatingPresetId::COMFORT, climate::CLIMATE_PRESET_COMFORT},
    {HeatingPresetId::ECO, climate::CLIMATE_PRESET_ECO},
    {HeatingPresetId::BOOST, climate::CLIMATE_PRESET_BOOST},
};

/// @brief UI target-temperature step. The wire carries 0.1 C (encode_heating_write() encodes a
/// 16-bit tenths-of-a-degree setpoint); 0.5 C is a UI-only choice for a sane thermostat card.
constexpr float CLIMATE_TARGET_TEMP_STEP_C = 0.5F;

climate::ClimateMode to_climate_mode(HeatingHvacMode hvac) {
  for (const auto &mapping : CLIMATE_MODE_MAP) {
    if (mapping.hvac == hvac)
      return mapping.climate_mode;
  }
  return climate::CLIMATE_MODE_OFF;
}

/// @return The ESPHome preset of a standard profile preset; CLIMATE_PRESET_NONE for a custom one.
climate::ClimatePreset to_climate_preset(HeatingPresetId id) {
  for (const auto &mapping : CLIMATE_PRESET_MAP) {
    if (mapping.id == id)
      return mapping.preset;
  }
  return climate::CLIMATE_PRESET_NONE;
}

}  // namespace

const HeatingProfile *IOHomeClimate::profile_() const {
  if (this->device_type_ != DeviceType::UNKNOWN)
    return find_heating_profile(this->device_type_, this->subtype_);
  // Declared without a type: use the one the hub has learned from the device since.
  const IoDevice *dev = this->parent_ != nullptr ? this->parent_->get_device(this->device_id_) : nullptr;
  return dev != nullptr ? find_heating_profile(dev->type, dev->subtype) : nullptr;
}

void IOHomeClimate::setup() {
  // Device gating, the single predicate. A known device this entity cannot drive (a non-climate
  // type, or a heater kind without a profile) is a configuration mistake, so fail it loudly rather
  // than let every command be rejected downstream. An UNKNOWN type is left to pass through: discovery
  // may still resolve it to a climate type, exactly as the other platforms tolerate unknown bindings.
  // This runs BEFORE register_device_binding_() so a misconfigured binding never injects a device
  // into the hub registry and never leaves a live callback on a failed entity.
  if (this->device_type_ != DeviceType::UNKNOWN &&
      !device_supports_climate_control(this->device_type_, this->subtype_)) {
    ESP_LOGE(TAG,
             "Device %s is declared as %s, which this climate entity cannot drive yet (it drives an electrical "
             "heater: io_device_type: electrical_heater, io_subtype: 0 or 1) — disabling it",
             this->device_id_.c_str(), format_device_kind(this->device_type_, this->subtype_).c_str());
    this->mark_failed();
    return;
  }

  // The initial poll reads the heater (IOHomeControlComponent::request_device_status()), so the card
  // shows measured values from the first minute instead of nothing.
  this->register_device_binding_(
      this, DeviceCapabilityClass::CLIMATE, std::nullopt,
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });

  if (this->has_range_)
    this->parent_->set_device_temperature_range(this->device_id_, this->range_min_c_, this->range_max_c_);

  // Custom presets are registered on the entity and referenced by the traits; IO's program mode,
  // the comfort offsets and the secured level have no standard preset.
  if (const HeatingProfile *profile = this->profile_(); profile != nullptr) {
    if (profile->features.target_temperature)
      this->max_temperature_c_ = profile->visual_max_c;
    for (size_t i = 0; i < profile->preset_count; i++) {
      if (profile->presets[i].id == HeatingPresetId::CUSTOM)
        this->custom_presets_.push_back(profile->presets[i].custom_name);
    }
    this->set_supported_custom_presets(this->custom_presets_);
  }
}

climate::ClimateTraits IOHomeClimate::traits() {
  climate::ClimateTraits traits;
  const HeatingProfile *profile = this->profile_();
  if (profile == nullptr) {
    // Type not known yet: the one mode every heater has.
    traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
    return traits;
  }

  // Both are filled only from a polled reply (sync_from_readings_()): a card that advertises them
  // shows an empty value until the first poll lands, which is the honest state.
  if (profile->features.current_temperature)
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  if (profile->features.action)
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION);
  for (size_t i = 0; i < profile->mode_count; i++)
    traits.add_supported_mode(to_climate_mode(profile->modes[i].hvac));
  for (size_t i = 0; i < profile->preset_count; i++) {
    if (profile->presets[i].id != HeatingPresetId::CUSTOM)
      traits.add_supported_preset(to_climate_preset(profile->presets[i].id));
  }
  // HEATING_TEMP_MIN_C up to the heater's own maximum setpoint once read, never past what
  // encode_heating_write() accepts (HEATING_TEMP_MAX_C).
  if (profile->features.target_temperature) {
    traits.set_visual_min_temperature(profile->visual_min_c);
    traits.set_visual_max_temperature(this->max_temperature_c_);
    traits.set_visual_target_temperature_step(CLIMATE_TARGET_TEMP_STEP_C);
  }
  return traits;
}

void IOHomeClimate::control(const climate::ClimateCall &call) {
  const HeatingProfile *profile = this->profile_();
  if (profile == nullptr) {
    ESP_LOGW(TAG, "Device %s: type not known yet, ignoring the request", this->device_id_.c_str());
    return;
  }
  // Each field is independent: a failed send for one leaves that field's state in place and does not
  // block the others. Every helper runs; publish once if any field advanced without a read-back.
  const bool mode_changed = this->apply_mode_(*profile, call);
  const bool custom_preset_changed = this->apply_custom_preset_(*profile, call);
  const bool preset_changed = this->apply_preset_(*profile, call);
  const bool temperature_changed = this->apply_target_temperature_(*profile, call);

  // A read-back publishes through on_device_update_(); this publishes what a write advanced when no
  // read-back has confirmed anything yet.
  if (mode_changed || custom_preset_changed || preset_changed || temperature_changed)
    this->publish_state();
}

bool IOHomeClimate::write_(const HeatingProfile &profile, HeatingFunction fn, uint8_t wire) {
  switch (profile.path) {
    case HeatingControlPath::HEATING_LEVEL:
      return this->parent_->set_heating_level(this->device_id_, wire);
    case HeatingControlPath::PRIVATE_REGISTERS:
      return this->parent_->send_heating_command(this->device_id_, fn, static_cast<float>(wire));
    case HeatingControlPath::TEMPERATURE_EXECUTE:
      break;  // its commands carry a main parameter and a functional parameter: write_preset_() and friends
  }
  return false;
}

bool IOHomeClimate::write_preset_(const HeatingProfile &profile, const HeatingPresetEntry &entry) {
  if (profile.path != HeatingControlPath::TEMPERATURE_EXECUTE)
    return this->write_(profile, entry.write_fn, entry.wire);
  // A setpoint alias is a main parameter, with the functional parameter the profile names for it.
  return this->parent_->set_heating_temperature(
      this->device_id_, heating_prefixed_main_parameter(profile.mp_prefix, entry.wire), entry.fp);
}

bool IOHomeClimate::encode_target_(const HeatingProfile &profile, float celsius, uint16_t &main_parameter) {
  const IoDevice *dev = this->parent_->get_device(this->device_id_);
  if (dev == nullptr)
    return false;
  // A profile with a fixed range never takes the YAML range, whatever the record holds.
  const float min_c = profile.fixed_range ? HEATING_TEMPERATURE_RANGE_MIN_C : dev->temperature_range_min_c;
  const float max_c = profile.fixed_range ? HEATING_TEMPERATURE_RANGE_MAX_C : dev->temperature_range_max_c;
  return encode_temperature_main_parameter(celsius, min_c, max_c, main_parameter);
}

bool IOHomeClimate::write_temperature_mode_(const HeatingProfile &profile, const HeatingModeEntry &entry) {
  uint16_t main_parameter = entry.main_parameter;
  if (main_parameter == HEATING_MP_FROM_TARGET) {
    // Heat (manual) is the current target temperature with the manual flag: the target the card shows,
    // else the one the device last reported.
    const IoDevice *dev = this->parent_->get_device(this->device_id_);
    float target = this->target_temperature;
    if (std::isnan(target) && dev != nullptr)
      target = dev->heating.setpoint_c;
    if (std::isnan(target) || !this->encode_target_(profile, target, main_parameter)) {
      ESP_LOGW(TAG, "Device %s: no target temperature known yet, set one before selecting heat",
               this->device_id_.c_str());
      return false;
    }
  }
  return this->parent_->set_heating_temperature(this->device_id_, main_parameter, entry.fp);
}

void IOHomeClimate::confirm_write_(HeatingFunction fn) {
  // A failed read is already logged by the hub, and the poll will find the register again; the
  // entity keeps whatever it showed. A level write needs no call here: the hub arms a settle poll.
  this->parent_->read_heating_register(this->device_id_, fn, nullptr, EXCHANGE_RETRY_COUNT);
}

bool IOHomeClimate::apply_mode_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &mode_opt = call.get_mode();
  if (!mode_opt.has_value())
    return false;
  const climate::ClimateMode requested = *mode_opt;
  for (size_t i = 0; i < profile.mode_count; i++) {
    const HeatingModeEntry &entry = profile.modes[i];
    if (to_climate_mode(entry.hvac) != requested)
      continue;
    const bool sent = profile.path == HeatingControlPath::TEMPERATURE_EXECUTE
                          ? this->write_temperature_mode_(profile, entry)
                          : this->write_(profile, HeatingFunction::SET_MODE, entry.wire);
    if (!sent)
      return false;
    // Until a read has succeeded there is nothing observed to show: the commanded mode stands in. A mode
    // that nothing reads back stays the last command.
    const bool commanded = !this->observed_ || !profile.features.mode_observed;
    this->mode_commanded_ = true;
    if (commanded) {
      this->mode = requested;
      // A real HVAC mode supersedes a preset on the register and level paths; a temperature heater's
      // preset follows its status reply instead.
      if (profile.path != HeatingControlPath::TEMPERATURE_EXECUTE) {
        this->clear_custom_preset_();
        this->preset.reset();
      }
    }
    if (profile.path == HeatingControlPath::PRIVATE_REGISTERS) {
      this->confirm_write_(HeatingFunction::SET_MODE);
      return commanded && !this->observed_;
    }
    return commanded;
  }
  return false;
}

bool IOHomeClimate::apply_custom_preset_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  if (!call.has_custom_preset())
    return false;
  const std::string custom = std::string(call.get_custom_preset());
  for (size_t i = 0; i < profile.preset_count; i++) {
    const HeatingPresetEntry &entry = profile.presets[i];
    if (entry.id != HeatingPresetId::CUSTOM || custom != entry.custom_name)
      continue;
    return this->commit_preset_(profile, entry);
  }
  return false;
}

bool IOHomeClimate::apply_preset_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &preset_opt = call.get_preset();
  if (!preset_opt.has_value())
    return false;
  const climate::ClimatePreset requested = *preset_opt;
  for (size_t i = 0; i < profile.preset_count; i++) {
    const HeatingPresetEntry &entry = profile.presets[i];
    if (entry.id == HeatingPresetId::CUSTOM || to_climate_preset(entry.id) != requested)
      continue;
    return this->commit_preset_(profile, entry);
  }
  return false;
}

bool IOHomeClimate::commit_preset_(const HeatingProfile &profile, const HeatingPresetEntry &entry) {
  if (!entry.commandable) {
    ESP_LOGW(TAG, "Device %s: preset '%s' is reported by the device and cannot be set", this->device_id_.c_str(),
             entry.custom_name != nullptr ? entry.custom_name : "(standard)");
    return false;
  }
  if (!this->write_preset_(profile, entry))
    return false;
  // Once a read has succeeded the card follows the read-back, not the command.
  const bool commanded = !this->observed_;
  if (commanded) {
    if (profile.features.mode_observed) {
      // The read-back uses the same table, so a commanded preset looks like a read one.
      this->apply_observed_state_(profile, entry.wire);
    } else if (entry.id == HeatingPresetId::CUSTOM) {
      this->set_custom_preset_(entry.custom_name);
    } else {
      this->preset = to_climate_preset(entry.id);
      this->clear_custom_preset_();             // a standard preset supersedes a custom one
      this->mode = climate::CLIMATE_MODE_HEAT;  // every alias preset heats
    }
  }
  if (profile.path == HeatingControlPath::PRIVATE_REGISTERS)
    this->confirm_write_(entry.write_fn);
  return commanded;
}

bool IOHomeClimate::apply_target_temperature_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &target_opt = call.get_target_temperature();
  if (!target_opt.has_value())
    return false;
  if (!profile.features.target_temperature) {
    ESP_LOGW(TAG, "Device %s has no adjustable setpoint; the request is ignored", this->device_id_.c_str());
    return false;
  }
  const float target = *target_opt;
  if (profile.path == HeatingControlPath::TEMPERATURE_EXECUTE) {
    // The temperature travels in the main parameter, encoded over the device's range; the settle poll
    // reads it back.
    uint16_t main_parameter = 0;
    if (!this->encode_target_(profile, target, main_parameter) ||
        !this->parent_->set_heating_temperature(this->device_id_, main_parameter, profile.setpoint_fp))
      return false;
    const bool commanded = !this->observed_;
    if (commanded) {
      this->target_temperature = target;
      this->clear_custom_preset_();
      this->preset.reset();
    }
    return commanded;
  }
  if (!this->parent_->send_heating_command(this->device_id_, HeatingFunction::SET_TEMPERATURE, target))
    return false;
  const bool commanded = !this->observed_;
  if (commanded)
    this->target_temperature = target;
  this->confirm_write_(HeatingFunction::SET_TEMPERATURE);
  return commanded && !this->observed_;
}

bool IOHomeClimate::sync_temperatures_(const HeatingProfile &profile, const HeatingReadings &readings, bool &seen) {
  bool changed = false;
  // NaN means "never read" (or a sensor fault): two NaNs are the same state, not a change.
  const auto differs = [](float a, float b) { return !(std::isnan(a) && std::isnan(b)) && a != b; };

  if (profile.features.current_temperature) {
    if (differs(this->current_temperature, readings.room_temperature_c)) {
      this->current_temperature = readings.room_temperature_c;
      changed = true;
    }
    seen = seen || !std::isnan(readings.room_temperature_c);
  }

  if (profile.features.target_temperature && !std::isnan(readings.setpoint_c)) {
    seen = true;
    if (differs(this->target_temperature, readings.setpoint_c)) {
      this->target_temperature = readings.setpoint_c;
      changed = true;
    }
  }

  // The heater's own ceiling, but never past what a write can encode.
  if (profile.features.target_temperature && !std::isnan(readings.max_setpoint_c) &&
      readings.max_setpoint_c > HEATING_TEMP_MIN_C) {
    seen = true;
    const float max_c = std::min(readings.max_setpoint_c, profile.visual_max_c);
    if (max_c != this->max_temperature_c_) {
      this->max_temperature_c_ = max_c;
      changed = true;
    }
  }
  return changed;
}

bool IOHomeClimate::sync_action_(const HeatingProfile &profile, const HeatingReadings &readings, bool mode_known,
                                 bool &seen) {
  if (!profile.features.action)
    return false;
  // The action follows the mode: an off heater is not "idle". Until the regulator has been read only
  // the off case is known.
  climate::ClimateAction action = this->action;
  if (this->mode == climate::CLIMATE_MODE_OFF && mode_known) {
    action = climate::CLIMATE_ACTION_OFF;
  } else if (readings.regulator >= 0) {
    seen = true;
    action = readings.regulator == static_cast<int16_t>(HeatingRegulatorState::HEATING)
                 ? climate::CLIMATE_ACTION_HEATING
                 : climate::CLIMATE_ACTION_IDLE;
  }
  if (action == this->action)
    return false;
  this->action = action;
  return true;
}

bool IOHomeClimate::sync_from_readings_(const HeatingProfile &profile, const HeatingReadings &readings) {
  bool seen = false;
  bool changed = this->sync_temperatures_(profile, readings, seen);

  // The mode register, or the heating level, whichever this profile's device reports.
  const int16_t wire = profile.path == HeatingControlPath::PRIVATE_REGISTERS ? readings.mode : readings.level;
  if (wire >= 0) {
    seen = true;
    changed = this->apply_observed_state_(profile, static_cast<uint8_t>(wire)) || changed;
  } else if (profile.path == HeatingControlPath::TEMPERATURE_EXECUTE && !std::isnan(readings.setpoint_c)) {
    // The device reports a plain temperature, so it is on no alias preset.
    if (this->has_custom_preset()) {
      this->clear_custom_preset_();
      changed = true;
    }
    if (this->preset.has_value()) {
      this->preset.reset();
      changed = true;
    }
  }
  changed = this->sync_action_(profile, readings, wire >= 0, seen) || changed;

  // Where nothing reads the mode back, a device that reports a setpoint or an alias is evidently on:
  // show heat until a mode is commanded, instead of the entity's default "off".
  if (!profile.features.mode_observed && !this->mode_commanded_ && seen && this->mode == climate::CLIMATE_MODE_OFF) {
    this->mode = climate::CLIMATE_MODE_HEAT;
    changed = true;
  }

  this->observed_ = this->observed_ || seen;
  return changed;
}

bool IOHomeClimate::apply_observed_state_(const HeatingProfile &profile, uint8_t wire) {
  const HeatingObservedState *observed = find_observed_state(profile, wire);
  if (observed == nullptr)
    return false;  // unknown value: leave the card as it was

  // A mode nothing reads back stays the last command; the table then only names the preset.
  bool changed = false;
  if (profile.features.mode_observed) {
    const climate::ClimateMode mode = to_climate_mode(observed->hvac);
    changed = mode != this->mode;
    this->mode = mode;
  }

  const HeatingPresetEntry *shown = observed->preset >= 0 ? &profile.presets[observed->preset] : nullptr;
  if (shown != nullptr && shown->id == HeatingPresetId::CUSTOM) {
    if (!this->has_custom_preset() || this->get_custom_preset() != shown->custom_name) {
      this->set_custom_preset_(shown->custom_name);
      changed = true;
    }
  } else {
    if (this->has_custom_preset()) {
      this->clear_custom_preset_();
      changed = true;
    }
    if (shown != nullptr) {
      const climate::ClimatePreset preset = to_climate_preset(shown->id);
      if (!this->preset.has_value() || *this->preset != preset) {
        this->preset = preset;
        changed = true;
      }
    } else if (this->preset.has_value()) {
      // The device reports it is on no preset.
      this->preset.reset();
      changed = true;
    }
  }
  return changed;
}

void IOHomeClimate::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;
  const HeatingProfile *profile = find_heating_profile(dev.type, dev.subtype);
  if (profile == nullptr)
    return;
  if (this->sync_from_readings_(*profile, dev.heating))
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
  // Only the register profile has run on hardware (an Atlantic AGILIA IO radiator, issue #165).
  const HeatingProfile *profile = this->profile_();
  const bool confirmed = profile != nullptr && profile->path == HeatingControlPath::PRIVATE_REGISTERS;
  ESP_LOGCONFIG(TAG, "  Status: %s",
                confirmed ? "confirmed on hardware (setpoint, mode, presets, read-back)"
                          : "experimental, unvalidated on hardware");
}

}  // namespace home_io_control
}  // namespace esphome
