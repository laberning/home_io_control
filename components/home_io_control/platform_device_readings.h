#pragma once

/// @file platform_device_readings.h
/// @brief The generic entities that show a named device reading: one numeric sensor and one binary sensor.
/// @ingroup hioc_platforms
///
/// A reading (DeviceReading, proto_device_readings.h) is what an entity shows; these entities show any of them.
/// The generated code names the reading with set_reading() and gives the entity its unit, device class, state class
/// and accuracy from the codegen mirror (hub_validators.py `DEVICE_READINGS`), so a new reading is a table row and,
/// at most, a codec, never a new entity class. They are device-bound companions (DeviceBoundCompanion): they read
/// through reading_value() when the device's record changes, add no radio traffic of their own, and publish only when
/// what they show changes.

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"
#include "proto_device_readings.h"

#include <optional>

namespace esphome {
namespace home_io_control {

/// @brief A device-bound companion that shows one named reading (the base of the entities below and of the heater
/// setting switch).
/// @ingroup hioc_platforms
class DeviceReadingCompanion : public DeviceBoundCompanion {
 public:
  /// @brief Name the reading this entity shows (generated code).
  /// @param reading A reading of the table.
  void set_reading(DeviceReading reading) { this->reading_ = reading; }

 protected:
  /// @brief The value of this entity's reading for a device.
  /// @param dev The device record.
  /// @return The value, or nullopt while it has not been read, was dropped or cannot be told.
  [[nodiscard]] std::optional<float> reading_value_(const IoDevice &dev) const {
    return reading_value(dev, this->reading_);
  }

  /// @return The reading's name for logs; "unknown" until the generated code names a reading of the table.
  [[nodiscard]] const char *reading_name_() const {
    const DeviceReadingRow *row = find_device_reading(this->reading_);
    return row != nullptr ? row->name : "unknown";
  }

  DeviceReading reading_{DeviceReading::COUNT};  ///< The reading; COUNT until the generated code names it.
};

/// @brief Sensor that shows one numeric reading of a device, with no state until the device has reported it.
/// @ingroup hioc_platforms
///
/// Used for the opt-in `energy:` key of a climate entry (the energy total, in kWh) and for the run timers of a towel
/// dryer (`boost_duration:`, `drying_duration:` and `remaining_time:`, in minutes). Nothing read, a reading dropped
/// and a device that answers without a value all leave the sensor without a state (NaN), never at 0. It publishes the
/// first update it sees and then only a changed value.
///
/// \todo Experimental except on a Sauter Tahual: the energy total equals that radiator's display (PR #238); compare
///       it with the display of an EMOTION 3 H, an Adelis and a Hekla.
/// \todo Experimental: the run timers are read on Atlantic Adelis units only (issue #202); `drying_duration:` is the
///       duration of the last drying run started, which a change on the panel alone does not update.
class IOHomeDeviceReadingSensor : public sensor::Sensor, public Component, public DeviceReadingCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  bool published_{false};  ///< Whether anything has been published yet.
  float shown_{NAN};       ///< The last value published; NaN is "no state".
};

/// @brief Binary sensor that shows one flag reading of a device, with no state until the device has reported it.
/// @ingroup hioc_platforms
///
/// Used for the opt-in `central_heating_mode:` and `occupancy:` keys of a climate entry. ON is 1 and OFF is 0, so
/// "off" always means the device said so: nothing read, a reading dropped and a device that answers without a value
/// (its register is absent) all leave the sensor without a state. It publishes only a changed state.
///
/// While a mixed towel dryer runs on central heating its electric element stays off, so the card shows idle and
/// Boost and Drying do nothing; `occupancy:` is on while the heater sees someone in the room.
///
/// \todo Experimental: the central-heating mode is read on one Atlantic Adelis mixed unit over four panel
///       toggles (issue #243); confirm on a second unit. The occupancy is read on a Sauter Hekla with the
///       detection on (issue #222); what it shows with the detection off is not confirmed, and no other heater
///       has answered it yet.
class IOHomeDeviceReadingBinarySensor : public binary_sensor::BinarySensor,
                                        public Component,
                                        public DeviceReadingCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  bool published_{false};      ///< Whether anything has been published yet.
  std::optional<bool> shown_;  ///< The last state published; nullopt is "no state".
};

}  // namespace home_io_control
}  // namespace esphome
