/// @file platform_device_sensor.cpp
/// @brief The `sensor:` entry that declares a device which has no other main entity, and schedules its readings.
/// @ingroup hioc_platforms

#include "platform_device_sensor.h"

#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.device_sensor";

void IOHomeDeviceSensor::setup() {
  // The entry shows no value of its own: its readings are separate entities that follow the device record, so the
  // update handler has nothing to publish.
  this->register_device_binding_(this, DeviceCapabilityClass::SENSOR, [](const IoDevice &) {});
  for (const ScheduledReading &scheduled : this->scheduled_readings_) {
    this->device_port_->set_device_reading_poll_interval(this->device_id_, scheduled.reading,
                                                         this->interval_of_(scheduled));
  }
}

void IOHomeDeviceSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol Device Sensor:");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  for (const ScheduledReading &scheduled : this->scheduled_readings_) {
    const DeviceReadingRow *row = find_device_reading(scheduled.reading);
    ESP_LOGCONFIG(TAG, "  Reading: %s every %" PRIu32 " s", row != nullptr ? row->name : "unknown",
                  this->interval_of_(scheduled) / 1000);
  }
}

}  // namespace home_io_control
}  // namespace esphome
