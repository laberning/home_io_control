/// @file platform_cover_controls.cpp
/// @brief Generated per-cover auxiliary controls: command button and silent-operation toggle.
/// @ingroup hioc_platforms

#include "platform_cover_controls.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

// One tag for the merged command button. The former per-button tags (home_io_control.favorite_button
// / home_io_control.vent_button) both retire with the class merge; a user greps for the entity by
// name, not by log tag, and this reads correctly for either command.
static const char *const TAG_COMMAND_BUTTON = "home_io_control.cover_command_button";
static const char *const TAG_SILENT = "home_io_control.cover.silent";

void IOHomeCoverCommandButton::press_action() {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG_COMMAND_BUTTON, "Ignoring %s press because the parent hub is not configured",
             cover_command_name(this->command_));
    return;
  }

  if (this->parent_->get_device(this->device_id_) == nullptr) {
    ESP_LOGW(TAG_COMMAND_BUTTON, "Ignoring %s press for %s because the device is not registered yet",
             cover_command_name(this->command_), this->device_id_.c_str());
    return;
  }

  this->parent_->queue_device_command(this->device_id_, this->command_);
}

void IOHomeCoverCommandButton::dump_config() {
  ESP_LOGCONFIG(TAG_COMMAND_BUTTON, "IO-Homecontrol %s Position Button", cover_command_name(this->command_));
  ESP_LOGCONFIG(TAG_COMMAND_BUTTON, "  Device ID: %s", this->device_id_.c_str());
}

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
  ESP_LOGI(TAG_SILENT, "Device %s: silent operation %s", this->device_id_.c_str(), state ? "on" : "off");
}

void IOHomeCoverSilentSwitch::dump_config() {
  ESP_LOGCONFIG(TAG_SILENT, "IO-Homecontrol Silent Operation:");
  ESP_LOGCONFIG(TAG_SILENT, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
