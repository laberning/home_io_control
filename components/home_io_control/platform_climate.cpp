/// @file platform_climate.cpp
/// @brief Experimental climate entity for IO-Homecontrol heating devices (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_platforms

#include "platform_climate.h"

#include "hub_internal.h"
#include "esphome/core/log.h"

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

/// @brief Custom preset name standing in for HeatingMode::PROG (there is no ClimateMode for a
/// device-side schedule).
constexpr const char *PROGRAM_PRESET_NAME = "Program";

/// @brief UI target-temperature step. The wire carries 0.1 C (encode_heating_payload() encodes a
/// 16-bit tenths-of-a-degree setpoint); 0.5 C is a UI-only choice for a sane thermostat card.
constexpr float CLIMATE_TARGET_TEMP_STEP_C = 0.5F;

}  // namespace

void IOHomeClimate::setup() {
  // Device-type gating, the single predicate. A known non-climate type is a configuration
  // mistake this entity can never drive, so fail it loudly rather than let every command be
  // rejected downstream. An UNKNOWN type is left to pass through: discovery may still resolve it
  // to a climate type, exactly as the other platforms tolerate unknown bindings. This runs
  // BEFORE register_device_binding_() so a misconfigured binding never injects a device into the
  // hub registry and never leaves a live callback on a failed entity.
  if (this->device_type_ != DeviceType::UNKNOWN && !device_supports_climate_control(this->device_type_)) {
    ESP_LOGE(TAG,
             "Device %s declares io_device_type '%s', which is not a climate device — disabling this climate entity",
             this->device_id_.c_str(), device_type_name(this->device_type_));
    this->mark_failed();
    return;
  }

  // No initial status poll: CMD_WRITE_PRIVATE is write-only, so a heating device has nothing to
  // read back and a status request would only draw a rejection warning at boot.
  this->register_device_binding_(
      this, /*inverted=*/false,
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); },
      /*schedule_initial_poll=*/false);

  // "Program" is a custom preset (registered on the entity, referenced by the traits) because IO's
  // prog mode has no ClimateMode equivalent.
  this->set_supported_custom_presets({PROGRAM_PRESET_NAME});
}

climate::ClimateTraits IOHomeClimate::traits() {
  climate::ClimateTraits traits;
  // No current-temperature stream: nothing in CMD_WRITE_PRIVATE or any decoded reply carries a
  // measured room temperature, so the entity leaves current_temperature unset (NAN) and never
  // advertises current-temperature support (the trait default).
  for (const auto &mapping : CLIMATE_MODE_MAP)
    traits.add_supported_mode(mapping.climate_mode);
  for (const auto &mapping : CLIMATE_PRESET_MAP)
    traits.add_supported_preset(mapping.preset);
  // 7.0-28.0 C: the range encode_heating_payload() accepts (see HEATING_TEMP_MAX_C — the setpoint
  // is a 16-bit LE tenths-of-a-degree field per the vendored Atlantic register map).
  traits.set_visual_min_temperature(HEATING_TEMP_MIN_C);
  traits.set_visual_max_temperature(HEATING_TEMP_MAX_C);
  traits.set_visual_target_temperature_step(CLIMATE_TARGET_TEMP_STEP_C);
  return traits;
}

void IOHomeClimate::control(const climate::ClimateCall &call) {
  // Each field is independent: a failed send for one leaves that field's last-commanded value in
  // place and does not block the others. Every helper runs; publish once if any field advanced.
  const bool mode_changed = this->apply_mode_(call);
  const bool custom_preset_changed = this->apply_custom_preset_(call);
  const bool preset_changed = this->apply_preset_(call);
  const bool temperature_changed = this->apply_target_temperature_(call);

  // Published state is "last commanded, never confirmed" — no reply ever verifies it.
  if (mode_changed || custom_preset_changed || preset_changed || temperature_changed)
    this->publish_state();
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
    this->mode = requested;
    this->clear_custom_preset_();  // a real HVAC mode supersedes the "Program" custom preset
    return true;
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
  this->set_custom_preset_(call.get_custom_preset());
  return true;
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
  this->target_temperature = target;
  return true;
}

void IOHomeClimate::on_device_update_(const std::string & /*id*/, const IoDevice & /*dev*/) {
  // Deliberately empty — a write-only heating device produces nothing this entity can publish.
  // Last Contact / Active Issue reach the user through the companion diagnostic sensors.
}

void IOHomeClimate::dump_config() {
  LOG_CLIMATE("", "IO-Homecontrol Climate", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Target temperature range: %.1f-%.1f C (documented max; write-only, never confirmed)",
                HEATING_TEMP_MIN_C, HEATING_TEMP_MAX_C);
  // No poll-interval line: heating is write-only and never polls (status_poll_interval is
  // rejected by climate.py).
  ESP_LOGCONFIG(TAG, "  Status: experimental, unvalidated on hardware");
}

}  // namespace home_io_control
}  // namespace esphome
