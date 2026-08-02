/// @file platform_light.cpp
/// @brief Binary or dimmable light entity for IO-Homecontrol devices.
/// @ingroup hioc_platforms

#include "platform_light.h"
#include "hub_internal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.light";

void IOHomeLight::setup() {
  // Lights need the shared device registry so discovery/status handling can find their node
  // metadata the same way covers do; the delayed initial poll matches the cover path.
  this->register_device_binding_(this, /*inverted=*/false, [this](const std::string &id, const IoDevice &dev) {
    this->on_device_update_(id, dev);
  });
  // `dimmable` is a light-only YAML choice, not part of the shared add_device() signature every
  // platform calls above — stamp it onto the now-registered device separately so profile-name
  // logging (see hub_operations.cpp's operation_profile_name()) can tell a dimmable light from a
  // plain binary one.
  this->parent_->set_device_dimmable(this->device_id_, this->dimmable_);
}

light::LightTraits IOHomeLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({this->dimmable_ ? light::ColorMode::BRIGHTNESS : light::ColorMode::ON_OFF});
  return traits;
}

void IOHomeLight::write_state(light::LightState *state) {
  if (this->suppress_write_) {
    this->suppress_write_ = false;
    return;
  }

  if (this->dimmable_) {
    float brightness = 0.0F;
    // Deliberately current_values.as_brightness(), not LightState::current_values_as_brightness()
    // — the latter applies gamma_correct (default 2.8 for a BRIGHTNESS_ONLY light), which is
    // meant for perceptually-correcting a PWM/LED output and silently mangles the value here (a
    // real bug caught on hardware: HA-set 50% arrived on the wire as ~15%, matching 0.5^2.8). We
    // want the exact linear percentage the user set — the Somfy device has its own dimming
    // behavior, if any, and gamma-correcting on top of that would be wrong regardless of curve.
    state->current_values.as_brightness(&brightness);
    // Convert HA brightness (0.0-1.0) to IO position (0-100) via detail::round_percent()
    // (hub_internal.h — rounds rather than truncates, see its doc comment): this device family's
    // convention is 0=full brightness, 100=off — the same inverted mapping platform_cover.cpp
    // uses for non-inverted covers (HA 1.0 open -> IO 0), confirmed on real dimmable hardware.
    auto const io_pos = detail::round_percent(1.0F - brightness);
    this->parent_->queue_set_light_position(this->device_id_, io_pos);
    return;
  }

  bool on = false;
  state->current_values_as_binary(&on);
  this->parent_->queue_set_light_state(this->device_id_, on);
}

void IOHomeLight::on_device_update_(const std::string &id, const IoDevice &dev) {
  // We only publish a stable HA state once the device reports a settled status (shared filter),
  // and only once the LightState object is available.
  if (this->state_ == nullptr || !this->passes_binary_update_filter_(id, dev))
    return;

  if (this->dimmable_) {
    // Same inverted 0-100 IO position field as the binary path, just read as a continuous value
    // instead of collapsed at the <50 threshold.
    const float brightness = 1.0F - (dev.position / 100.0F);
    const bool on = brightness > 0.0F;
    if (this->state_->current_values.is_on() == on && this->state_->remote_values.is_on() == on &&
        this->state_->current_values.get_brightness() == brightness &&
        this->state_->remote_values.get_brightness() == brightness)
      return;

    this->suppress_write_ = true;
    auto call = this->state_->make_call();
    call.set_state(on);
    call.set_brightness(brightness);
    call.set_save(false);
    call.perform();
    return;
  }

  // Binary endpoints report on/off via the shared 0-100 position field.
  // Position < 50 is treated as "on", >= 50 as "off".
  const bool on = dev.position < detail::BINARY_ENTITY_ON_POSITION_THRESHOLD;
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
  ESP_LOGCONFIG(TAG, "IO-Homecontrol %s Light", this->dimmable_ ? "Dimmable" : "Binary");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  this->log_poll_interval_config_(TAG);
}

}  // namespace home_io_control
}  // namespace esphome