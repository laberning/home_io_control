/// @file platform_climate_controls.cpp
/// @brief Generated per-heater auxiliary entities: the window binary sensor, the heater setting switches
/// (open-window detection, occupancy detection), the night light and the heating-level text sensor.
/// @ingroup hioc_platforms

#include "platform_climate_controls.h"

#include "entity_helpers.h"
#include "esphome/core/log.h"
#include "proto_heating_profiles.h"

#include <algorithm>
#include <optional>

namespace esphome {
namespace home_io_control {

static const char *const TAG_WINDOW = "home_io_control.window";
static const char *const TAG_SETTING = "home_io_control.heater_setting";
static const char *const TAG_NIGHT_LIGHT = "home_io_control.night_light";
static const char *const TAG_HEATING_LEVEL = "home_io_control.heating_level";

/// Percentage for a light switched on at a brightness that rounds to 0 %: the lowest level that is still
/// on (register value night_light_register_from_percent(1), 30).
static constexpr uint8_t NIGHT_LIGHT_MIN_ON_PERCENT = 1;

void IOHomeHeatingLevelSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // Nothing read means no state, never a guessed level; a value that is no level keeps the last one shown.
    if (dev.heating.level < 0)
      return;
    const char *name = heating_level_name(static_cast<uint8_t>(dev.heating.level));
    if (name != nullptr)
      this->publish_state(name);
  });
}

void IOHomeHeatingLevelSensor::dump_config() {
  ESP_LOGCONFIG(TAG_HEATING_LEVEL, "IO-Homecontrol Heating Level:");
  ESP_LOGCONFIG(TAG_HEATING_LEVEL, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeWindowBinarySensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (dev.heating.window_open < 0) {
      // "Closed" must mean the heater said so: nothing read means no state.
      this->invalidate_state();
      return;
    }
    this->publish_state(dev.heating.window_open != 0);
  });
}

void IOHomeWindowBinarySensor::dump_config() {
  ESP_LOGCONFIG(TAG_WINDOW, "IO-Homecontrol Window:");
  ESP_LOGCONFIG(TAG_WINDOW, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeHeaterSettingSwitch::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // Shown only from a reply; a switch with no reading yet keeps its unknown state.
    if (const std::optional<float> value = this->reading_value_(dev))
      this->publish_state(*value != 0.0F);
  });
}

void IOHomeHeaterSettingSwitch::write_state(bool state) {
  const std::optional<HeatingFunction> setting = reading_register(this->reading_);
  if (this->heating_port_ == nullptr || !setting)
    return;
  if (!this->heating_port_->send_heating_command(this->device_id_, *setting, state ? 1.0F : 0.0F)) {
    ESP_LOGW(TAG_SETTING, "Device %s: %s %s was not acknowledged", this->device_id_.c_str(), this->reading_name_(),
             state ? "on" : "off");
    return;
  }
  // The read-back updates IoDevice::heating, and the companion binding above publishes it.
  this->heating_port_->read_heating_register(this->device_id_, *setting, nullptr, EXCHANGE_RETRY_COUNT);
}

void IOHomeHeaterSettingSwitch::dump_config() {
  ESP_LOGCONFIG(TAG_SETTING, "IO-Homecontrol Heater Setting:");
  ESP_LOGCONFIG(TAG_SETTING, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG_SETTING, "  Reading: %s", this->reading_name_());
}

void IOHomeHeaterNightLight::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) { this->on_device_update_(dev); });
}

light::LightTraits IOHomeHeaterNightLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
  return traits;
}

void IOHomeHeaterNightLight::on_device_update_(const IoDevice &dev) {
  // Shown only from a reply: a light with no reading yet keeps its unknown state.
  if (this->state_ == nullptr || this->writing_ || dev.heating.night_light < 0)
    return;
  const bool on = dev.heating.night_light != 0;
  // A brightness of 0 (or none read yet) says nothing about the level the light comes on at: keep the
  // one shown.
  const float brightness =
      dev.heating.night_light_brightness > 0
          ? night_light_fraction_from_register(static_cast<uint16_t>(dev.heating.night_light_brightness))
          : this->state_->remote_values.get_brightness();
  publish_light_reading(this->state_, on, brightness, this->suppress_write_);
}

bool IOHomeHeaterNightLight::write_register_(HeatingFunction fn, float value) {
  if (this->heating_port_->send_heating_command(this->device_id_, fn, value))
    return true;
  ESP_LOGW(TAG_NIGHT_LIGHT, "Device %s: %s %.0f was not acknowledged", this->device_id_.c_str(),
           heating_function_name(fn), value);
  return false;
}

void IOHomeHeaterNightLight::write_state(light::LightState *state) {
  if (this->suppress_write_) {
    this->suppress_write_ = false;
    return;
  }
  if (this->heating_port_ == nullptr || this->device_port_ == nullptr)
    return;

  this->writing_ = true;
  const bool on = state->current_values.is_on();
  bool brightness_written = false;
  if (on) {
    // The raw brightness, not current_values_as_brightness(): that one applies gamma_correct, meant for a
    // PWM output, and would send a different percentage than the one the user picked.
    const uint8_t percent = std::clamp<uint8_t>(detail::round_percent(state->current_values.get_brightness()),
                                                NIGHT_LIGHT_MIN_ON_PERCENT, HEATING_NIGHT_LIGHT_PERCENT_MAX);
    const uint16_t brightness = night_light_register_from_percent(percent);
    const IoDevice *dev = this->device_port_->get_device(this->device_id_);
    // Compared as the percentage Home Assistant shows, not as the register value: the panel's own levels
    // (100, 1000) fall between two whole percentages, and switching on at the brightness shown must not
    // move the heater off its level.
    const bool holds_percent = dev != nullptr && dev->heating.night_light_brightness > 0 &&
                               detail::round_percent(night_light_fraction_from_register(
                                   static_cast<uint16_t>(dev->heating.night_light_brightness))) == percent;
    if (!holds_percent)
      brightness_written = this->write_register_(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, brightness);
  }
  // On/off goes out even when the brightness write failed: switching the light is what the user asked.
  const bool switched = this->write_register_(HeatingFunction::NIGHT_LIGHT, on ? 1.0F : 0.0F);

  // The read-backs update IoDevice::heating; what the heater then holds is published once, below. A write
  // the heater did not acknowledge is not read back: the light then shows the heater's last reading again.
  if (switched) {
    this->heating_port_->read_heating_register(this->device_id_, HeatingFunction::NIGHT_LIGHT, nullptr,
                                               EXCHANGE_RETRY_COUNT);
  }
  if (brightness_written) {
    this->heating_port_->read_heating_register(this->device_id_, HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, nullptr,
                                               EXCHANGE_RETRY_COUNT);
  }
  this->writing_ = false;
  if (const IoDevice *dev = this->device_port_->get_device(this->device_id_); dev != nullptr)
    this->on_device_update_(*dev);
}

void IOHomeHeaterNightLight::dump_config() {
  ESP_LOGCONFIG(TAG_NIGHT_LIGHT, "IO-Homecontrol Heater Night Light:");
  ESP_LOGCONFIG(TAG_NIGHT_LIGHT, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
