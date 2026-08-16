/// @file platform_cover_silent_switch.cpp
/// @brief Silent-operation toggle implementation.
/// @ingroup hioc_platforms

#include "platform_cover_silent_switch.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.cover.silent";

void IOHomeCoverSilentSwitch::setup() {
  // The YAML value was already applied to the device at registration; mirror it outward so Home
  // Assistant does not start out disagreeing with what the hub will actually send.
  if (this->parent_ == nullptr)
    return;
  if (const IoDevice *dev = this->parent_->get_device(this->device_id_); dev != nullptr)
    this->publish_state(dev->silent);
}

void IOHomeCoverSilentSwitch::write_state(bool state) {
  if (this->parent_ == nullptr)
    return;
  this->parent_->set_device_silent(this->device_id_, state);
  this->publish_state(state);
  ESP_LOGI(TAG, "Device %s: silent operation %s", this->device_id_.c_str(), state ? "on" : "off");
}

void IOHomeCoverSilentSwitch::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Silent Operation:");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
