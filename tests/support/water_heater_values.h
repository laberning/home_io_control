#pragma once

/// @file water_heater_values.h
/// @brief Test helpers that put a water heater's values into its device record and read them back, whichever way
/// the hub keeps them.
///
/// A test arranges what the hub has read from a water heater with seed_water_heater() and
/// store_water_heater_values(), and checks what it kept with water_heater_values(). Every field starts as "not
/// read": NaN for a temperature or an energy, -1 for a whole number.

#include "heater_controller.h"
#include "proto_device_model.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_value_codecs.h"
#include "reading_store.h"

#include <cmath>
#include <cstdint>

namespace test {

using namespace esphome::home_io_control;

/// The low byte of a water heater register's number as it travels in the frame (family 0x00), written out here so a
/// request is checked against the wire and not against the table that built it.
constexpr uint8_t dhw_register_byte(DeviceRegister fn) {
  switch (fn) {
    case DeviceRegister::DHW_BOOST_DURATION:
      return 0x00;
    case DeviceRegister::DHW_ENERGY:
      return 0x04;
    case DeviceRegister::DHW_AWAY_DURATION:
      return 0x05;
    case DeviceRegister::DHW_MODE:
      return 0x06;
    case DeviceRegister::DHW_BRAND:
      return 0x07;
    case DeviceRegister::DHW_WATER_TEMPERATURE:
      return 0x17;
    case DeviceRegister::DHW_HEATING:
      return 0x21;
    default:
      return 0xFF;
  }
}

/// What a water heater's record holds, as plain numbers. Unset (NaN, -1) means "not read".
struct WaterHeaterValues {
  float water_c{NAN};      ///< Water temperature.
  float target_c{NAN};     ///< Target temperature.
  int mode{-1};            ///< DhwMode value.
  int heating{-1};         ///< Something heats, 0 or 1.
  int element{-1};         ///< The electric backup runs, 0 or 1.
  int heat_pump{-1};       ///< The heat pump runs, 0 or 1.
  int capabilities{-1};    ///< FP15 word.
  int boost{-1};           ///< Boost on, 0 or 1.
  int away{-1};            ///< Away on, 0 or 1.
  int boost_duration{-1};  ///< Register 0x0000.
  int away_duration{-1};   ///< Register 0x0005.
  float energy_kwh{NAN};   ///< Energy in kWh, as the reading holds it.
  int brand{-1};           ///< Brand letter.
};

/// Let a water heater record read one status reply the way the hub does (apply_dhw_status_reply(), the position
/// trusted), so a test never repeats the glue between the reply's bytes and the readings.
inline void apply_status_reply(IoDevice &dev, const IoFrame &reply) {
  apply_dhw_status_reply("ABC123", dev, reply, true);
}

/// What the record holds now.
inline WaterHeaterValues water_heater_values(const IoDevice &dev) {
  const auto number = [&dev](DeviceReading reading) { return reading_value(dev, reading).value_or(NAN); };
  const auto wire = [&dev](DeviceReading reading) { return reading_wire(dev, reading); };
  return {.water_c = number(DeviceReading::HOT_WATER_TEMPERATURE),
          .target_c = number(DeviceReading::STATUS_SETPOINT),
          .mode = wire(DeviceReading::HOT_WATER_MODE),
          .heating = wire(DeviceReading::HOT_WATER_HEATING),
          .element = wire(DeviceReading::HOT_WATER_ELECTRIC_BACKUP),
          .heat_pump = wire(DeviceReading::HOT_WATER_HEAT_PUMP),
          .capabilities = wire(DeviceReading::HOT_WATER_CAPABILITIES),
          .boost = wire(DeviceReading::HOT_WATER_BOOST),
          .away = wire(DeviceReading::HOT_WATER_AWAY),
          .boost_duration = wire(DeviceReading::HOT_WATER_BOOST_DURATION),
          .away_duration = wire(DeviceReading::HOT_WATER_AWAY_DURATION),
          .energy_kwh = number(DeviceReading::HOT_WATER_ENERGY),
          .brand = wire(DeviceReading::HOT_WATER_BRAND)};
}

/// Replace every value of the record: a field left unset is "not read".
inline void store_water_heater_values(IoDevice &dev, const WaterHeaterValues &v) {
  const auto number = [&dev](DeviceReading reading, float value) {
    if (std::isnan(value)) {
      dev.reading_values.clear(reading_slot(reading));
    } else {
      dev.reading_values.store(reading_slot(reading), value);
    }
  };
  const auto wire = [&dev](DeviceReading reading, int value) { store_wire(dev, reading, value); };
  number(DeviceReading::HOT_WATER_TEMPERATURE, v.water_c);
  number(DeviceReading::STATUS_SETPOINT, v.target_c);
  wire(DeviceReading::HOT_WATER_MODE, v.mode);
  wire(DeviceReading::HOT_WATER_HEATING, v.heating);
  wire(DeviceReading::HOT_WATER_ELECTRIC_BACKUP, v.element);
  wire(DeviceReading::HOT_WATER_HEAT_PUMP, v.heat_pump);
  wire(DeviceReading::HOT_WATER_CAPABILITIES, v.capabilities);
  wire(DeviceReading::HOT_WATER_BOOST, v.boost);
  wire(DeviceReading::HOT_WATER_AWAY, v.away);
  wire(DeviceReading::HOT_WATER_BOOST_DURATION, v.boost_duration);
  wire(DeviceReading::HOT_WATER_AWAY_DURATION, v.away_duration);
  number(DeviceReading::HOT_WATER_ENERGY, v.energy_kwh);
  wire(DeviceReading::HOT_WATER_BRAND, v.brand);
}

/// Keep the fields that are set and leave every other value as it was.
inline void seed_water_heater(IoDevice &dev, const WaterHeaterValues &v) {
  WaterHeaterValues merged = water_heater_values(dev);
  if (!std::isnan(v.water_c))
    merged.water_c = v.water_c;
  if (!std::isnan(v.target_c))
    merged.target_c = v.target_c;
  const auto keep = [](int &field, int seed) {
    if (seed >= 0)
      field = seed;
  };
  keep(merged.mode, v.mode);
  keep(merged.heating, v.heating);
  keep(merged.element, v.element);
  keep(merged.heat_pump, v.heat_pump);
  keep(merged.capabilities, v.capabilities);
  keep(merged.boost, v.boost);
  keep(merged.away, v.away);
  keep(merged.boost_duration, v.boost_duration);
  keep(merged.away_duration, v.away_duration);
  keep(merged.brand, v.brand);
  if (!std::isnan(v.energy_kwh))
    merged.energy_kwh = v.energy_kwh;
  store_water_heater_values(dev, merged);
}

}  // namespace test
