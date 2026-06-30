/// @file platform_cover_vent_button.cpp
/// @brief Generated ventilation-position button for window-type cover entities.
/// @ingroup hioc_platforms

#include "platform_cover_vent_button.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.vent_button";

void IOHomeCoverVentButton::press_action() {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG, "Ignoring ventilation press because the parent hub is not configured");
    return;
  }

  if (this->parent_->get_device(this->device_id_) == nullptr) {
    ESP_LOGW(TAG, "Ignoring ventilation press for %s because the device is not registered yet",
             this->device_id_.c_str());
    return;
  }

  this->parent_->queue_device_command(this->device_id_, CoverCommand::VENT);
}

void IOHomeCoverVentButton::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Ventilation Position Button");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
