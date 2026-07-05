/// @file platform_switch.cpp
/// @brief Experimental binary switch entity for IO-Homecontrol devices.
/// @ingroup hioc_platforms

#include "platform_switch.h"
#include "hub_internal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.switch";

void IOHomeSwitch::setup() {
  // Register and subscribe exactly like covers so the controller keeps one shared view of all
  // known IO-homecontrol devices, regardless of which ESPHome entity type wraps them.
  this->register_device_binding_(this, /*inverted=*/false, [this](const std::string &id, const IoDevice &dev) {
    this->on_device_update_(id, dev);
  });
}

void IOHomeSwitch::write_state(bool state) {
  // Switches stay semantic at the entity boundary and let the controller translate them to the
  // transport-level 0/100 representation.
  this->parent_->queue_set_switch_state(this->device_id_, state);
}

void IOHomeSwitch::on_device_update_(const std::string &id, const IoDevice &dev) {
  // Only publish a stable state for this device once it has stopped moving with a known position.
  if (!this->passes_binary_update_filter_(id, dev))
    return;

  // Binary endpoints share the same on/off encoding as the light wrapper.
  this->publish_state(dev.position < detail::BINARY_ENTITY_ON_POSITION_THRESHOLD);
}

void IOHomeSwitch::dump_config() {
  LOG_SWITCH("", "IO-Homecontrol Binary Switch", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  this->log_poll_interval_config_(TAG);
  ESP_LOGCONFIG(TAG, "  Status: experimental and untested");
}

}  // namespace home_io_control
}  // namespace esphome