/// @file platform_last_seen_sensor.cpp
/// @brief Diagnostic sensor exposing seconds-since-boot of a device's last received frame.
/// @ingroup hioc_platforms

#include "platform_last_seen_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.last_seen";

namespace {

constexpr float MS_PER_SECOND = 1000.0F;

}  // namespace

void IOHomeLastSeenSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (dev.last_seen_ms != 0)
      this->publish_state(static_cast<float>(dev.last_seen_ms) / MS_PER_SECOND);
  });
}

void IOHomeLastSeenSensor::dump_config() {
  LOG_SENSOR("", "IO-Homecontrol Last Seen", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
