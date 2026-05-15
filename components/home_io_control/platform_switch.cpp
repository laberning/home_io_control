/// @file platform_switch.cpp
/// @brief Experimental binary switch entity for IO-Homecontrol devices.

#include "platform_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.switch";

void IOHomeSwitch::setup() {
  // Register and subscribe exactly like covers so the controller keeps one shared view of all
  // known IO-homecontrol devices, regardless of which ESPHome entity type wraps them.
  this->parent_->add_device(this->device_id_);
  this->parent_->register_device_callback(
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });
  this->set_timeout("init_status", 5000, [this]() { this->parent_->queue_request_device_status(this->device_id_); });
}

void IOHomeSwitch::write_state(bool state) {
  // Switches stay semantic at the entity boundary and let the controller translate them to the
  // transport-level 0/100 representation.
  this->parent_->queue_set_switch_state(this->device_id_, state);
}

void IOHomeSwitch::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;

  // Like the light entity, only publish a stable state once the device has stopped moving.
  if (dev.position == UNKNOWN_POSITION || !dev.is_stopped)
    return;

  // Binary endpoints share the same on/off encoding as the light wrapper.
  this->publish_state(dev.position < 50.0F);
}

void IOHomeSwitch::dump_config() {
  LOG_SWITCH("", "IO-Homecontrol Binary Switch", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Status: experimental and untested");
}

}  // namespace home_io_control
}  // namespace esphome