/// @file platform_climate_controls.cpp
/// @brief Generated per-heater auxiliary entities: the window binary sensor, the open-window
/// detection switch and the night light.
/// @ingroup hioc_platforms

#include "platform_climate_controls.h"

#include "entity_helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <utility>

namespace esphome {
namespace home_io_control {

static const char *const TAG_WINDOW = "home_io_control.window";
static const char *const TAG_DETECTION = "home_io_control.window_detection";
static const char *const TAG_NIGHT_LIGHT = "home_io_control.night_light";

/// Brightness register value for a light switched on at a brightness that rounds to 0 %: the lowest
/// level that is still on.
static constexpr uint8_t NIGHT_LIGHT_MIN_ON_PERCENT = 1;

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

void IOHomeOpenWindowDetectionSwitch::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // Shown only from a reply; a switch with no reading yet keeps its unknown state.
    if (dev.heating.open_window_detection >= 0)
      this->publish_state(dev.heating.open_window_detection != 0);
  });
}

void IOHomeOpenWindowDetectionSwitch::write_state(bool state) {
  if (this->parent_ == nullptr)
    return;
  if (!this->parent_->send_heating_command(this->device_id_, HeatingFunction::SET_OPEN_WINDOW_DETECTION,
                                           state ? 1.0F : 0.0F)) {
    ESP_LOGW(TAG_DETECTION, "Device %s: open-window detection %s was not acknowledged", this->device_id_.c_str(),
             state ? "on" : "off");
    return;
  }
  // The read-back updates IoDevice::heating, and the companion binding above publishes it.
  this->parent_->read_heating_register(this->device_id_, HeatingFunction::SET_OPEN_WINDOW_DETECTION, nullptr,
                                       EXCHANGE_RETRY_COUNT);
}

void IOHomeOpenWindowDetectionSwitch::dump_config() {
  ESP_LOGCONFIG(TAG_DETECTION, "IO-Homecontrol Open Window Detection:");
  ESP_LOGCONFIG(TAG_DETECTION, "  Device ID: %s", this->device_id_.c_str());
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
          ? static_cast<float>(dev.heating.night_light_brightness) / HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX
          : this->state_->remote_values.get_brightness();
  publish_light_reading(this->state_, on, brightness, this->suppress_write_);
}

bool IOHomeHeaterNightLight::write_register_(HeatingFunction fn, float value) {
  if (this->parent_->send_heating_command(this->device_id_, fn, value))
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
  if (this->parent_ == nullptr)
    return;

  this->writing_ = true;
  const bool on = state->current_values.is_on();
  bool brightness_written = false;
  if (on) {
    // The raw brightness, not current_values_as_brightness(): that one applies gamma_correct, meant for a
    // PWM output, and would send a different percentage than the one the user picked.
    const uint8_t percent = std::clamp<uint8_t>(detail::round_percent(state->current_values.get_brightness()),
                                                NIGHT_LIGHT_MIN_ON_PERCENT, HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX);
    const IoDevice *dev = this->parent_->get_device(this->device_id_);
    if (dev == nullptr || std::cmp_not_equal(dev->heating.night_light_brightness, percent))
      brightness_written = this->write_register_(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, percent);
  }
  // On/off goes out even when the brightness write failed: switching the light is what the user asked.
  const bool switched = this->write_register_(HeatingFunction::NIGHT_LIGHT, on ? 1.0F : 0.0F);

  // The read-backs update IoDevice::heating; what the heater then holds is published once, below. A write
  // the heater did not acknowledge is not read back: the light then shows the heater's last reading again.
  if (switched)
    this->parent_->read_heating_register(this->device_id_, HeatingFunction::NIGHT_LIGHT, nullptr, EXCHANGE_RETRY_COUNT);
  if (brightness_written) {
    this->parent_->read_heating_register(this->device_id_, HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, nullptr,
                                         EXCHANGE_RETRY_COUNT);
  }
  this->writing_ = false;
  if (const IoDevice *dev = this->parent_->get_device(this->device_id_); dev != nullptr)
    this->on_device_update_(*dev);
}

void IOHomeHeaterNightLight::dump_config() {
  ESP_LOGCONFIG(TAG_NIGHT_LIGHT, "IO-Homecontrol Heater Night Light:");
  ESP_LOGCONFIG(TAG_NIGHT_LIGHT, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
