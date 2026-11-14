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

climate::ClimateAction to_climate_action(HeatingAction action) {
  switch (action) {
    case HeatingAction::HEATING:
      return climate::CLIMATE_ACTION_HEATING;
    case HeatingAction::IDLE:
      return climate::CLIMATE_ACTION_IDLE;
    case HeatingAction::OFF:
      break;
  }
  return climate::CLIMATE_ACTION_OFF;
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
  const IoDevice *dev = this->device_port_ != nullptr ? this->device_port_->get_device(this->device_id_) : nullptr;
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

  // The initial poll reads the heater (HubDevicePort::queue_request_device_status()), so the card
  // shows measured values from the first minute instead of nothing.
  this->register_device_binding_(this, DeviceCapabilityClass::CLIMATE,
                                 [this](const IoDevice &dev) { this->on_device_update_(dev); });

  if (this->has_range_)
    this->heating_port_->set_device_temperature_range(this->device_id_, this->range_min_c_, this->range_max_c_);

  // Custom presets are registered on the entity and referenced by the traits; IO's program mode,
  // the comfort offsets and the secured level have no standard preset.
  if (const HeatingProfile *profile = this->profile_(); profile != nullptr) {
    this->card_ = initial_heating_card(*profile);
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

  // Both are filled only from a polled reply (reduce_heating_card()): a card that advertises them
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
    traits.set_visual_max_temperature(this->card_.max_target_c);
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
  this->show_card_(*profile);

  // A read-back publishes through on_device_update_(); this publishes what a write advanced when no
  // read-back has confirmed anything yet.
  if (mode_changed || custom_preset_changed || preset_changed || temperature_changed)
    this->publish_state();
}

HeatingWritePlan IOHomeClimate::plan_write_(const HeatingProfile &profile, const HeatingWriteRequest &request) {
  const HeatingWritePlan plan =
      plan_heating_write(profile, request, heating_device_facts(this->device_port_->get_device(this->device_id_)));
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
  HeatingHvacMode hvac{};
  if (!to_hvac_mode(*mode_opt, hvac))
    return false;
  const HeatingWriteRequest request{
      .kind = HeatingWriteKind::MODE, .hvac = hvac, .shown_target_c = this->card_.target_c};
  return this->send_and_show_(profile, request);
}

bool IOHomeClimate::apply_custom_preset_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  if (!call.has_custom_preset())
    return false;
  const std::string custom = std::string(call.get_custom_preset());
  return this->send_and_show_(
      profile, {.kind = HeatingWriteKind::PRESET, .preset = HeatingPresetId::CUSTOM, .custom_name = custom.c_str()});
}

bool IOHomeClimate::apply_preset_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &preset_opt = call.get_preset();
  if (!preset_opt.has_value())
    return false;
  HeatingPresetId id{};
  if (!to_preset_id(*preset_opt, id))
    return false;
  return this->send_and_show_(profile, {.kind = HeatingWriteKind::PRESET, .preset = id});
}

bool IOHomeClimate::apply_target_temperature_(const HeatingProfile &profile, const climate::ClimateCall &call) {
  const auto &target_opt = call.get_target_temperature();
  if (!target_opt.has_value())
    return false;
  return this->send_and_show_(profile, {.kind = HeatingWriteKind::TARGET, .target_c = *target_opt});
}

bool IOHomeClimate::send_and_show_(const HeatingProfile &profile, const HeatingWriteRequest &request) {
  const HeatingWritePlan plan = this->plan_write_(profile, request);
  if (!plan.is_write())
    return false;

  bool shown = false;
  const bool sent = this->heating_port_->execute_heating_plan(this->device_id_, plan, [&] {
    const HeatingCardCommand command = command_heating_card(profile, request, plan, this->card_);
    this->card_ = command.card;
    shown = command.shown;
  });
  if (!sent)
    return false;
  // A mode or target written to a register is read back, and a read-back that made the card observed
  // publishes itself. A preset publishes with the command, as it always has.
  const bool read_back_publishes = plan.read_back && request.kind != HeatingWriteKind::PRESET;
  return read_back_publishes ? shown && !this->card_.observed : shown;
}

void IOHomeClimate::show_card_(const HeatingProfile &profile) {
  this->mode = to_climate_mode(this->card_.mode);
  this->current_temperature = this->card_.current_c;
  this->target_temperature = this->card_.target_c;
  this->action = to_climate_action(this->card_.action);
  // The readings are folded with the device record's profile and commands with the entity's; should a
  // misdeclared type make the two differ, an index from one must never read past the other's list.
  const bool known_index =
      this->card_.preset_index >= 0 && static_cast<size_t>(this->card_.preset_index) < profile.presets.size();
  const HeatingPresetEntry *shown = known_index ? &profile.presets[this->card_.preset_index] : nullptr;
  if (shown != nullptr && shown->id == HeatingPresetId::CUSTOM) {
    if (!this->has_custom_preset() || this->get_custom_preset() != shown->custom_name)
      this->set_custom_preset_(shown->custom_name);
    return;
  }
  this->clear_custom_preset_();
  if (shown != nullptr) {
    this->preset = to_climate_preset(shown->id);
  } else {
    this->preset.reset();
  }
}

void IOHomeClimate::on_device_update_(const IoDevice &dev) {
  const HeatingProfile *profile = find_heating_profile(dev);
  if (profile == nullptr)
    return;
  const HeatingCardState next = reduce_heating_card(*profile, dev.heating, this->card_);
  const bool changed = !next.shows_same_as(this->card_);
  this->card_ = next;
  this->show_card_(*profile);
  if (changed)
    this->publish_state();
}

void IOHomeClimate::dump_config() {
  LOG_CLIMATE("", "IO-Homecontrol Climate", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Target temperature range: %.1f-%.1f C (maximum from the heater once read)", HEATING_TEMP_MIN_C,
                this->card_.max_target_c);
  this->log_poll_interval_config_(TAG, heater_poll_interval_ms);
  if (const DeviceTypeInfo *model = find_device_model_info(this->device_type_, this->subtype_, this->model_))
    ESP_LOGCONFIG(TAG, "  Model: %s", model->name);
  // Only the register profiles have run on hardware: an Atlantic AGILIA IO radiator (issue #165) and an
  // Atlantic Adelis towel dryer (issue #202), both all the way.
  const char *status = "experimental, unvalidated on hardware";
  switch (heating_hardware_status(this->profile_())) {
    case HeatingHardwareStatus::WRITES_EXPERIMENTAL:
      status = "readings and setpoint confirmed on hardware; mode and preset writes experimental";
      break;
    case HeatingHardwareStatus::CONFIRMED:
      status = "confirmed on hardware (setpoint, mode, presets, read-back)";
      break;
    case HeatingHardwareStatus::UNVALIDATED:
      break;
  }
  ESP_LOGCONFIG(TAG, "  Status: %s", status);
}

}  // namespace home_io_control
}  // namespace esphome
