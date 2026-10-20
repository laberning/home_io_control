/// @file platform_climate_controls.cpp
/// @brief Generated per-heater auxiliary entities: the window binary sensor and the open-window
/// detection switch.
/// @ingroup hioc_platforms

#include "platform_climate_controls.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG_WINDOW = "home_io_control.window";
static const char *const TAG_DETECTION = "home_io_control.window_detection";

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

}  // namespace home_io_control
}  // namespace esphome
