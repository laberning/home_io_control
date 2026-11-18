/// @file platform_device_readings.cpp
/// @brief The generic entities that show a named device reading: a numeric sensor, a binary sensor and a text sensor.
/// @ingroup hioc_platforms

#include "platform_device_readings.h"

#include "esphome/core/log.h"

#include <cmath>
#include <cstdint>

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

void IOHomeDeviceReadingTextSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    const DeviceReadingRow *row = find_device_reading(this->reading_);
    // Nothing read means no state, never a guessed name; a value with no name keeps the one shown.
    const int value = reading_wire(dev, this->reading_);
    if (row == nullptr || row->value_name == nullptr || value < 0 || value > UINT8_MAX)
      return;
    const char *name = row->value_name(static_cast<uint8_t>(value));
    // A name is never empty, so the unset state (empty) never equals one.
    if (name != nullptr && this->state != name)
      this->publish_state(name);
  });
}

void IOHomeDeviceReadingTextSensor::dump_config() {
  ESP_LOGCONFIG(TAG_READING, "IO-Homecontrol Device Reading Text Sensor:");
  ESP_LOGCONFIG(TAG_READING, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG_READING, "  Reading: %s", this->reading_name_());
}

}  // namespace home_io_control
}  // namespace esphome
