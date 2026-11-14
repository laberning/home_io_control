/// @file platform_climate.cpp
/// @brief Experimental climate entity for IO-Homecontrol heaters, driven by a heating profile.
/// @ingroup hioc_platforms

#include "platform_climate.h"

#include "entity_helpers.h"
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
    {HeatingPresetId::NONE, climate::CLIMATE_PRESET_NONE},       {HeatingPresetId::AWAY, climate::CLIMATE_PRESET_AWAY},
    {HeatingPresetId::COMFORT, climate::CLIMATE_PRESET_COMFORT}, {HeatingPresetId::ECO, climate::CLIMATE_PRESET_ECO},
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

/// @param[out] hvac The profile mode of an ESPHome mode.
/// @return false for a mode no profile offers (cool, dry, fan only, heat/cool).
bool to_hvac_mode(climate::ClimateMode mode, HeatingHvacMode &hvac) {
  for (const auto &mapping : CLIMATE_MODE_MAP) {
    if (mapping.climate_mode == mode) {
      hvac = mapping.hvac;
      return true;
    }
  }
  return false;
}

/// @param[out] id The profile preset of an ESPHome standard preset.
/// @return false for a preset no profile offers.
bool to_preset_id(climate::ClimatePreset preset, HeatingPresetId &id) {
  for (const auto &mapping : CLIMATE_PRESET_MAP) {
    if (mapping.preset == preset) {
      id = mapping.id;
      return true;
    }
  }
  return false;
}

}  // namespace

const HeatingProfile *IOHomeClimate::profile_() const {
  if (this->device_type_ != DeviceType::UNKNOWN)
    return find_heating_profile(this->device_type_, this->subtype_, this->model_);
  // Declared without a type: use the one the hub has learned from the device since.
  const IoDevice *dev = this->parent_ != nullptr ? this->parent_->get_device(this->device_id_) : nullptr;
  return dev != nullptr ? find_heating_profile(*dev) : nullptr;
}

void IOHomeClimate::setup() {
  // Device gating, the single predicate. A known device this entity cannot drive (a non-climate
  // type, or a heater kind without a profile) is a configuration mistake, so fail it loudly rather
  // than let every command be rejected downstream. An UNKNOWN type is left to pass through: discovery
  // may still resolve it to a climate type, exactly as the other platforms tolerate unknown bindings.
  // This runs BEFORE register_device_binding_() so a misconfigured binding never injects a device
  // into the hub registry and never leaves a live callback on a failed entity.
  if (this->reject_undrivable_kind_(TAG, device_supports_climate_control(this->device_type_, this->subtype_),
                                    "which has no heating profile, so this climate entity cannot drive it")) {
    this->mark_failed();
    return;
  }

  // The initial poll reads the heater (IOHomeControlComponent::request_device_status()), so the card
  // shows measured values from the first minute instead of nothing.
  this->register_device_binding_(this, DeviceCapabilityClass::CLIMATE,
                                 [this](const IoDevice &dev) { this->on_device_update_(dev); });

  if (this->has_range_)
    this->parent_->set_device_temperature_range(this->device_id_, this->range_min_c_, this->range_max_c_);

  // Custom presets are registered on the entity and referenced by the traits; IO's program mode,
  // the comfort offsets and the secured level have no standard preset.
  if (const HeatingProfile *profile = this->profile_(); profile != nullptr) {
    if (profile->features.target_temperature)
      this->max_temperature_c_ = profile->visual_max_c;
    for (const HeatingPresetEntry &entry : profile->presets) {
      if (entry.id == HeatingPresetId::CUSTOM)
        this->custom_presets_.push_back(entry.custom_name);
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
  for (const HeatingModeEntry &entry : profile->modes)
    traits.add_supported_mode(to_climate_mode(entry.hvac));
  for (const HeatingPresetEntry &entry : profile->presets) {
    if (entry.id != HeatingPresetId::CUSTOM)
      traits.add_supported_preset(to_climate_preset(entry.id));
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

HeatingWritePlan IOHomeClimate::plan_write_(const HeatingProfile &profile, const HeatingWriteRequest &request) {
  const HeatingWritePlan plan =
      plan_heating_write(profile, request, heating_device_facts(this->parent_->get_device(this->device_id_)));
  if (!plan.is_write())
    this->log_refusal_(profile, plan);
  return plan;
}

void IOHomeClimate::log_refusal_(const HeatingProfile &profile, const HeatingWritePlan &plan) const {
  switch (plan.refusal) {
    case HeatingRefusal::NOT_COMMANDABLE: {
      const char *name = profile.presets[plan.preset_index].custom_name;
      ESP_LOGW(TAG, "Device %s: preset '%s' is reported by the device and cannot be set", this->device_id_.c_str(),
               name != nullptr ? name : "(standard)");
      break;
    }
    case HeatingRefusal::NO_SETPOINT:
      ESP_LOGW(TAG, "Device %s has no adjustable setpoint; the request is ignored", this->device_id_.c_str());
      break;
    case HeatingRefusal::NO_TARGET:
      ESP_LOGW(TAG, "Device %s: no target temperature known yet, set one before selecting heat",
               this->device_id_.c_str());
      break;
    case HeatingRefusal::NO_MATCH:     // the request names nothing this profile offers: nothing to say
    case HeatingRefusal::UNENCODABLE:  // the setpoint is dropped silently, as the card keeps its value
    case HeatingRefusal::NONE:
      break;
  }
}

bool IOHomeClimate::apply_mode_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &mode_opt = call.get_mode();
  if (!mode_opt.has_value())
    return false;
  const climate::ClimateMode requested = *mode_opt;
  HeatingHvacMode hvac{};
  if (!to_hvac_mode(requested, hvac))
    return false;
  const HeatingWritePlan plan = this->plan_write_(
      profile, {.kind = HeatingWriteKind::MODE, .hvac = hvac, .shown_target_c = this->target_temperature});
  if (!plan.is_write())
    return false;

  bool commanded = false;
  const bool sent = this->parent_->execute_heating_plan(this->device_id_, plan, [&] {
    // Until a read has succeeded there is nothing observed to show: the commanded mode stands in. A mode
    // that nothing reads back stays the last command.
    commanded = !this->observed_ || !profile.features.mode_observed;
    this->mode_commanded_ = true;
    if (!commanded)
      return;
    this->mode = requested;
    // A real HVAC mode supersedes a preset on the register and level paths; a temperature heater's
    // preset follows its status reply instead, and so does a preset kept in a register of its own
    // (a towel dryer's boost outlives a mode change).
    if (profile.path != HeatingControlPath::TEMPERATURE_EXECUTE && profile.preset_observed.empty()) {
      this->clear_custom_preset_();
      this->preset.reset();
    }
  });
  if (!sent)
    return false;
  // A read-back may have made the card observed in the meantime: then it publishes itself.
  return plan.read_back ? commanded && !this->observed_ : commanded;
}

bool IOHomeClimate::apply_custom_preset_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  if (!call.has_custom_preset())
    return false;
  const std::string custom = std::string(call.get_custom_preset());
  return this->commit_preset_(
      profile, {.kind = HeatingWriteKind::PRESET, .preset = HeatingPresetId::CUSTOM, .custom_name = custom.c_str()});
}

bool IOHomeClimate::apply_preset_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &preset_opt = call.get_preset();
  if (!preset_opt.has_value())
    return false;
  HeatingPresetId id{};
  if (!to_preset_id(*preset_opt, id))
    return false;
  return this->commit_preset_(profile, {.kind = HeatingWriteKind::PRESET, .preset = id});
}

bool IOHomeClimate::commit_preset_(const HeatingProfile &profile, const HeatingWriteRequest &request) {
  const HeatingWritePlan plan = this->plan_write_(profile, request);
  if (!plan.is_write())
    return false;
  const HeatingPresetEntry &entry = profile.presets[plan.preset_index];

  bool commanded = false;
  const bool sent = this->parent_->execute_heating_plan(this->device_id_, plan, [&] {
    // Once a read has succeeded the card follows the read-back, not the command.
    commanded = !this->observed_;
    if (!commanded)
      return;
    if (!profile.preset_observed.empty()) {
      // The preset has a register and a table of its own: its value is never a mode value.
      this->apply_observed_preset_(profile, entry.wire);
    } else if (profile.features.mode_observed) {
      // The read-back uses the same table, so a commanded preset looks like a read one.
      this->apply_observed_state_(profile, entry.wire);
    } else if (entry.id == HeatingPresetId::CUSTOM) {
      this->set_custom_preset_(entry.custom_name);
    } else {
      this->preset = to_climate_preset(entry.id);
      this->clear_custom_preset_();             // a standard preset supersedes a custom one
      this->mode = climate::CLIMATE_MODE_HEAT;  // every alias preset heats
    }
  });
  return sent && commanded;
}

bool IOHomeClimate::apply_target_temperature_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &target_opt = call.get_target_temperature();
  if (!target_opt.has_value())
    return false;
  const float target = *target_opt;
  const HeatingWritePlan plan = this->plan_write_(profile, {.kind = HeatingWriteKind::TARGET, .target_c = target});
  if (!plan.is_write())
    return false;

  bool commanded = false;
  const bool sent = this->parent_->execute_heating_plan(this->device_id_, plan, [&] {
    commanded = !this->observed_;
    if (!commanded)
      return;
    this->target_temperature = target;
    if (plan.kind == HeatingPlanKind::TEMPERATURE) {
      // A temperature heater's alias preset ends with the explicit setpoint; the settle poll reads it back.
      this->clear_custom_preset_();
      this->preset.reset();
    }
  });
  if (!sent)
    return false;
  return plan.read_back ? commanded && !this->observed_ : commanded;
}

bool IOHomeClimate::sync_temperatures_(const HeatingProfile &profile, const HeatingReadings &readings, float target_c,
                                       bool &seen) {
  bool changed = false;
  if (profile.features.current_temperature) {
    if (detail::reading_changed(this->current_temperature, readings.room_temperature_c)) {
      this->current_temperature = readings.room_temperature_c;
      changed = true;
    }
    seen = seen || !std::isnan(readings.room_temperature_c);
  }

  if (profile.features.target_temperature && !std::isnan(target_c)) {
    seen = true;
    if (detail::reading_changed(this->target_temperature, target_c)) {
      this->target_temperature = target_c;
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
  // The action follows the mode: an off heater is not "idle". Until the profile's action source has
  // been read only the off case is known.
  climate::ClimateAction action = this->action;
  if (this->mode == climate::CLIMATE_MODE_OFF && mode_known) {
    action = climate::CLIMATE_ACTION_OFF;
  } else if (profile.action_source == HeatingActionSource::REGULATOR && readings.regulator >= 0) {
    seen = true;
    action = readings.regulator == static_cast<int16_t>(HeatingRegulatorState::HEATING)
                 ? climate::CLIMATE_ACTION_HEATING
                 : climate::CLIMATE_ACTION_IDLE;
  } else if (profile.action_source == HeatingActionSource::WORKING_RATE && readings.working_rate >= 0) {
    seen = true;
    action = readings.working_rate > 0 ? climate::CLIMATE_ACTION_HEATING : climate::CLIMATE_ACTION_IDLE;
  }
  if (action == this->action)
    return false;
  this->action = action;
  return true;
}

bool IOHomeClimate::sync_from_readings_(const HeatingProfile &profile, const HeatingReadings &readings) {
  bool seen = false;
  // While the heater's own programme runs, its comfort or eco period and the temperature it heats to
  // replace the temporary-state preset and the comfort setpoint.
  const ProgrammePeriodState period = resolve_programme_period(profile, readings);
  const bool period_shown = period.active && !std::isnan(period.target_c);
  bool changed =
      this->sync_temperatures_(profile, readings, period_shown ? period.target_c : readings.setpoint_c, seen);

  // The profile's mode register, or the heating level, whichever this profile's device reports.
  const int16_t wire =
      profile.path == HeatingControlPath::PRIVATE_REGISTERS ? readings.value_of(profile.mode_fn) : readings.level;
  if (wire >= 0) {
    seen = true;
    changed = this->apply_observed_state_(profile, static_cast<uint8_t>(wire)) || changed;
  }
  // The period of the programme that runs, else the preset kept in a register of its own (a towel dryer's
  // temporary state; with the programme running and no period named, that is "none").
  const int16_t preset_wire = !profile.preset_observed.empty() ? readings.value_of(profile.preset_fn) : -1;
  if (period_shown && period.preset_index >= 0) {
    seen = true;
    changed = this->show_preset_(profile, period.preset_index) || changed;
  } else if (preset_wire >= 0) {
    seen = true;
    changed = this->apply_observed_preset_(profile, static_cast<uint8_t>(preset_wire)) || changed;
  }
  if (wire < 0 && profile.path == HeatingControlPath::TEMPERATURE_EXECUTE && !std::isnan(readings.setpoint_c)) {
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
  // With a preset table of its own, the mode register says nothing about the preset.
  if (!profile.preset_observed.empty())
    return changed;
  return this->show_preset_(profile, observed->preset) || changed;
}

bool IOHomeClimate::apply_observed_preset_(const HeatingProfile &profile, uint8_t wire) {
  const HeatingObservedState *observed = find_observed_preset(profile, wire);
  if (observed == nullptr)
    return false;  // unknown value: leave the card as it was
  return this->show_preset_(profile, observed->preset);
}

bool IOHomeClimate::show_preset_(const HeatingProfile &profile, int8_t preset_index) {
  bool changed = false;
  const HeatingPresetEntry *shown = preset_index >= 0 ? &profile.presets[preset_index] : nullptr;
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

void IOHomeClimate::on_device_update_(const IoDevice &dev) {
  const HeatingProfile *profile = find_heating_profile(dev);
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
  this->log_poll_interval_config_(TAG, heater_poll_interval_ms);
  if (const DeviceTypeInfo *model = find_device_model_info(this->device_type_, this->subtype_, this->model_))
    ESP_LOGCONFIG(TAG, "  Model: %s", model->name);
  // Only the register profiles have run on hardware: an Atlantic AGILIA IO radiator (issue #165) and an
  // Atlantic Adelis towel dryer (issue #202), both all the way.
  const HeatingProfile *profile = this->profile_();
  const bool registers = profile != nullptr && profile->path == HeatingControlPath::PRIVATE_REGISTERS;
  const char *status = "experimental, unvalidated on hardware";
  if (registers) {
    status = profile->experimental_writes
                 ? "readings and setpoint confirmed on hardware; mode and preset writes experimental"
                 : "confirmed on hardware (setpoint, mode, presets, read-back)";
  }
  ESP_LOGCONFIG(TAG, "  Status: %s", status);
}

}  // namespace home_io_control
}  // namespace esphome
