/// @file platform_rssi_sensor.cpp
/// @brief Diagnostic sensor exposing a device's smoothed RSSI.
/// @ingroup hioc_platforms

#include "platform_rssi_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.rssi";

void IOHomeRssiSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (const int16_t ema_dbm = device_rssi_ema_dbm(dev); ema_dbm != RSSI_UNKNOWN_DBM)
      this->publish_state(ema_dbm);
  });
}

void IOHomeRssiSensor::dump_config() {
  LOG_SENSOR("", "IO-Homecontrol RSSI", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
