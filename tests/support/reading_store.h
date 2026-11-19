#pragma once

/// @file reading_store.h
/// @brief Test helpers that put values into a device's reading store and read them back, by reading or by register.
///
/// The hub keeps every value of a device in IoDevice::reading_values, by reading (DeviceReading). A test arranges
/// what the hub has read with store_reading() and checks what it kept with number_read() and wire_read(); both name
/// either the reading or the register of a heater, whichever the test is about.

#include "proto_device_readings.h"

#include <cmath>
#include <cstdint>

namespace test {

using namespace esphome::home_io_control;

/// Keep a value for a reading, as a read would; a NaN is a value (a sensor the device reports as faulty).
template<class T> void store_reading(IoDevice &dev, DeviceReading reading, T value) {
  dev.reading_values.store(reading_slot(reading), static_cast<float>(value));
}

/// Keep a value for the reading that is a register of a heater.
template<class T> void store_reading(IoDevice &dev, DeviceRegister fn, T value) {
  store_register_reading(dev, fn, static_cast<float>(value));
}

/// Keep a whole-number value for a reading, or forget it when `wire` is negative (the register shows nothing).
inline void store_wire(IoDevice &dev, DeviceReading reading, int wire) {
  if (wire < 0) {
    dev.reading_values.clear(reading_slot(reading));
  } else {
    dev.reading_values.store(reading_slot(reading), static_cast<float>(wire));
  }
}

/// Forget the value of a reading: it reads as never read again.
inline void clear_reading(IoDevice &dev, DeviceReading reading) { dev.reading_values.clear(reading_slot(reading)); }

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
  const size_t slot = reading_slot(row->reading);
  if (dev.reading_values.absent(slot))
    return ReadState::ABSENT;
  return dev.reading_values.asked(slot) ? ReadState::ASKED : ReadState::NOT_ASKED;
}

/// Does the device hold a value for the reading that is this register?
inline bool has_reading(const IoDevice &dev, DeviceRegister fn) { return register_value(dev, fn).has_value(); }

}  // namespace test
