#pragma once

/// @file reading_store.h
/// @brief Test helpers that put values into a device's reading store and read them back, by reading or by register.
///
/// The hub keeps every value of a device in IoDevice::reading_values, by reading (DeviceReading). A test arranges
/// what the hub has read with store_reading() and checks what it kept with number_read() and wire_read(); both name
/// either the reading or the register of a heater, whichever the test is about.

#include "proto_device_readings.h"
#include "proto_heating_profiles.h"

#include <cmath>
#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

namespace test {

using namespace esphome::home_io_control;

/// Keep a value for a reading, as a read would; a NaN is a value (a sensor the device reports as faulty).
template<class T> void store_reading(IoDevice &dev, DeviceReading reading, T value) {
  esphome::home_io_control::store_reading(dev, reading, static_cast<float>(value));
}

/// Keep a value for the reading that is a register of a heater.
template<class T> void store_reading(IoDevice &dev, DeviceRegister fn, T value) {
  esphome::home_io_control::store_reading(dev, find_register_reading(fn)->reading, static_cast<float>(value));
}

/// Keep a whole-number value for a reading, or forget it when `wire` is negative (the register shows nothing).
inline void store_wire(IoDevice &dev, DeviceReading reading, int wire) {
  if (wire < 0) {
    clear_reading(dev, reading);
  } else {
    esphome::home_io_control::store_reading(dev, reading, static_cast<float>(wire));
  }
}

/// Forget the value of the reading that is a register.
inline void clear_reading(IoDevice &dev, DeviceRegister fn) { clear_register_reading(dev, fn); }

/// The value of a reading, or NaN when it has none.
inline float number_read(const IoDevice &dev, DeviceReading reading) {
  return reading_value(dev, reading).value_or(NAN);
}

/// The value of the reading that is a register, or NaN when it has none.
inline float number_read(const IoDevice &dev, DeviceRegister fn) { return register_value(dev, fn).value_or(NAN); }

/// The value of a reading as a whole number, or -1 when it has none.
inline int wire_read(const IoDevice &dev, DeviceReading reading) { return reading_wire(dev, reading); }

/// The value of the reading that is a register as a whole number, or -1 when it has none.
inline int wire_read(const IoDevice &dev, DeviceRegister fn) { return register_wire(dev, fn); }

/// Whether the hub's poll reads a reading for an entity.
enum class ReadState : uint8_t {
  NOT_ASKED,  ///< No entity asked for the reading.
  ASKED,      ///< An entity asked for it: every poll reads it.
  ABSENT,     ///< The device answered it without a value: the hub reads it no more.
};

/// The state of the reading that is a register.
inline ReadState read_state(const IoDevice &dev, DeviceRegister fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  if (row == nullptr)
    return ReadState::NOT_ASKED;
  if (reading_absent(dev, row->reading))
    return ReadState::ABSENT;
  return reading_asked(dev, row->reading) ? ReadState::ASKED : ReadState::NOT_ASKED;
}

/// Does the device hold a value for the reading that is this register?
inline bool has_reading(const IoDevice &dev, DeviceRegister fn) { return register_value(dev, fn).has_value(); }

/// The reading a profile's programme keeps its temperature in force in, or nullptr for a device without a programme.
inline const DeviceReadingRow *in_force_row_of(const IoDevice &dev) {
  const HeatingProfile *profile = find_heating_profile(dev);
  return profile != nullptr && profile->programme_period != nullptr
             ? find_register_reading(profile->programme_period->in_force_fn)
             : nullptr;
}

/// What the device's record says about its temperature in force (in_force_support()).
inline InForceSupport in_force_of(const IoDevice &dev) {
  const HeatingProfile *profile = find_heating_profile(dev);
  return profile != nullptr ? in_force_support(*profile, dev) : InForceSupport::UNKNOWN;
}

/// Put a device into one of the three in-force states, as the poll would leave it. REPORTED keeps a value the reading
/// holds and holds none otherwise; UNKNOWN forgets the state and the value and keeps every other reading.
inline void set_in_force_support(IoDevice &dev, InForceSupport support) {
  const DeviceReadingRow *row = in_force_row_of(dev);
  ASSERT_NE(row, nullptr) << "the device's profile has no programme";
  const std::optional<float> held = reading_value(dev, row->reading);
  if (support == InForceSupport::UNKNOWN) {
    // The store keeps "reported" for the boot, so a fresh store takes over every other reading.
    IoDevice fresh{};
    for (size_t i = 0; i < static_cast<size_t>(DeviceReading::COUNT); i++) {
      const auto reading = static_cast<DeviceReading>(i);
      if (reading == row->reading)
        continue;
      if (const auto value = reading_value(dev, reading); value && dev.reading_values.value(i))
        esphome::home_io_control::store_reading(fresh, reading, *value);
      if (reading_absent(dev, reading))
        mark_reading_absent(fresh, reading);
      if (reading_asked(dev, reading))
        ask_reading(fresh, reading);
    }
    dev.reading_values = fresh.reading_values;
    return;
  }
  if (support == InForceSupport::ABSENT) {
    mark_reading_absent(dev, row->reading);
    return;
  }
  esphome::home_io_control::store_reading(dev, row->reading, held.value_or(0.0F));
  if (!held)
    clear_reading(dev, row->reading);
}

}  // namespace test
