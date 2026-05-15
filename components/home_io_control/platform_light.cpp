/// @file platform_light.cpp
/// @brief Experimental binary light entity for IO-Homecontrol devices.

#include "platform_light.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.light";

void IOHomeLight::setup() {
  // Binary lights still need the shared device registry so discovery/status handling can find
  // their node metadata the same way covers do.
  this->parent_->add_device(this->device_id_);

  // Subscribe to controller updates so HA state follows radio-originated changes too.
  this->parent_->register_device_callback(
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });

  // Ask for one delayed poll after boot, matching the cover platform behavior.
  this->set_timeout("init_status", 5000, [this]() { this->parent_->queue_request_device_status(this->device_id_); });
}

light::LightTraits IOHomeLight::get_traits() {
  auto traits = light::LightTraits();
  // Dimming is intentionally not exposed yet; would need real device to verify.
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void IOHomeLight::write_state(light::LightState *state) {
  bool on = false;
  state->current_values_as_binary(&on);

  if (this->suppress_write_) {
    this->suppress_write_ = false;
    return;
  }

  this->parent_->queue_set_light_state(this->device_id_, on);
}

void IOHomeLight::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_ || this->state_ == nullptr)
    return;

  // Ignore unknown or in-flight positions. For binary devices we only want to publish a stable
  // HA state once the device reports a settled status.
  if (dev.position == UNKNOWN_POSITION || !dev.is_stopped)
    return;

  // Binary endpoints report on/off via the shared 0-100 position field.
  // Position < 50 is treated as "on", >= 50 as "off".
  const bool on = dev.position < 50.0F;
  if (this->state_->current_values.is_on() == on && this->state_->remote_values.is_on() == on)
    return;

  // Update via LightState's call API, suppressing the write_state() echo to avoid re-sending.
  this->suppress_write_ = true;
  auto call = this->state_->make_call();
  call.set_state(on);
  call.set_save(false);
  call.perform();
}

void IOHomeLight::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Binary Light");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Status: experimental and untested");
}

}  // namespace home_io_control
}  // namespace esphome