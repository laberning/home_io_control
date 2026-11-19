/// @file platform_device_sensor.cpp
/// @brief The `sensor:` entry that declares a device which has no other main entity, and schedules its readings.
/// @ingroup hioc_platforms

#include "platform_device_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.device_sensor";

void IOHomeDeviceSensor::setup() {
  // The entry shows no value of its own: its readings are separate entities that follow the device record, so the
  // update handler has nothing to publish.
  this->register_device_binding_(this, DeviceCapabilityClass::SENSOR, [](const IoDevice &) {});
  this->schedule_readings_(this->update_interval_ms_);
}

void IOHomeDeviceSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Device Sensor:");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  this->log_scheduled_readings_(TAG, this->update_interval_ms_);
}

}  // namespace home_io_control
}  // namespace esphome
