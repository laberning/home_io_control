/// @file platform_device_readings.cpp
/// @brief The generic entities that show a named device reading: one numeric sensor and one binary sensor.
/// @ingroup hioc_platforms

#include "platform_device_readings.h"

#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace home_io_control {

static const char *const TAG_READING = "home_io_control.device_reading";

void IOHomeDeviceReadingSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // NaN is "no state": a counter never reads as 0 unless the device said so.
    const float value = this->reading_value_(dev).value_or(NAN);
    // Equal readings, and two "no state"s (NaN is never equal to itself), publish nothing.
    if (this->published_ && (value == this->shown_ || (std::isnan(value) && std::isnan(this->shown_))))
      return;
    this->published_ = true;
    this->shown_ = value;
    this->publish_state(value);
  });
}

void IOHomeDeviceReadingSensor::dump_config() {
  ESP_LOGCONFIG(TAG_READING, "IO-Homecontrol Device Reading Sensor:");
  ESP_LOGCONFIG(TAG_READING, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG_READING, "  Reading: %s", this->reading_name_());
}

void IOHomeDeviceReadingBinarySensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // "Off" must mean the device said so: nothing read, or a reading dropped, means no state.
    const std::optional<float> value = this->reading_value_(dev);
    std::optional<bool> state;
    if (value)
      state = *value != 0.0F;
    if (this->published_ && state == this->shown_)
      return;
    this->published_ = true;
    this->shown_ = state;
    if (state) {
      this->publish_state(*state);
    } else {
      this->invalidate_state();
    }
  });
}

void IOHomeDeviceReadingBinarySensor::dump_config() {
  ESP_LOGCONFIG(TAG_READING, "IO-Homecontrol Device Reading Binary Sensor:");
  ESP_LOGCONFIG(TAG_READING, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG_READING, "  Reading: %s", this->reading_name_());
}

}  // namespace home_io_control
}  // namespace esphome
