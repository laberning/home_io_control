/// @file platform_exchange_failures_sensor.cpp
/// @brief Diagnostic sensor exposing a device's cumulative exchange-timeout count.
/// @ingroup hioc_platforms

#include "platform_exchange_failures_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.exchange_failures";

void IOHomeExchangeFailuresSensor::setup() {
  this->register_companion_binding_(
      [this](const IoDevice &dev) { this->publish_state(static_cast<float>(dev.exchange_timeout_count)); });
}

void IOHomeExchangeFailuresSensor::dump_config() {
  LOG_SENSOR("", "IO-Homecontrol Exchange Failures", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
