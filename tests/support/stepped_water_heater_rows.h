#pragma once

/// @file stepped_water_heater_rows.h
/// @brief Test-only registers and reading rows in the shape of a water heater whose boost, away and target are
/// registers, and whose target takes only steps.
///
/// Production has no such heater yet (every row of the reading table is the Atlantic Calypso's), so the shape lives
/// here: a one-byte target register that counts showers (2, 3 and 4 stand for 50, 60 and 70 degrees), written only in
/// manual mode, a register the target is read from, and a flag register each for boost and away. The registers are
/// DeviceRegister values above the production ones; a test makes them known with SteppedHeaterTables, which replaces
/// the register table and the reading table for its lifetime.

#include "proto_device_features.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_heating.h"
#include "proto_value_codecs.h"

#include <array>
#include <cstdint>

namespace test::stepped {

using namespace esphome::home_io_control;

// === The registers ===

/// The register the target is written to, as a shower count (low byte 0x48).
constexpr auto STEP_REGISTER = static_cast<DeviceRegister>(0xF0);
/// The register the temperature the heater heats to is read from (low byte 0x49).
constexpr auto TARGET_REGISTER = static_cast<DeviceRegister>(0xF1);
/// The boost flag register (low byte 0x52).
constexpr auto BOOST_REGISTER = static_cast<DeviceRegister>(0xF2);
/// The away flag register (low byte 0x53).
constexpr auto AWAY_REGISTER = static_cast<DeviceRegister>(0xF3);

/// Showers 2, 3 and 4 stand for 50, 60 and 70 degrees.
inline constexpr ValueScaleKnot SHOWER_KNOTS[] = {{50, 2}, {60, 3}, {70, 4}};
static_assert(value_scale_rises(SHOWER_KNOTS));

/// The registers of the shape, and the water heater's mode register the target's guard reads.
inline constexpr DeviceRegisterRow REGISTERS[] = {
    {.name = "test_mode",
     .fn = DeviceRegister::DHW_MODE,
     .register_low = 0x06,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::BOUNDED_BYTE,
     .max_value = 2,
     .register_high = DHW_REGISTER_HIGH_BYTE},
    {.name = "test_step",
     .fn = STEP_REGISTER,
     .register_low = 0x48,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::SCALED_BYTE,
     .max_value = 4,
     .register_high = DHW_REGISTER_HIGH_BYTE,
     .scale = SHOWER_KNOTS},
    {.name = "test_target",
     .fn = TARGET_REGISTER,
     .register_low = 0x49,
     .access = HeatingAccess::READ_ONLY,
     .codec = ValueCodec::BOUNDED_BYTE,
     .max_value = 100,
     .register_high = DHW_REGISTER_HIGH_BYTE},
    {.name = "test_boost",
     .fn = BOOST_REGISTER,
     .register_low = 0x52,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::BINARY,
     .register_high = DHW_REGISTER_HIGH_BYTE},
    {.name = "test_away",
     .fn = AWAY_REGISTER,
     .register_low = 0x53,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::BINARY,
     .register_high = DHW_REGISTER_HIGH_BYTE},
};

// === The reading rows ===

/// The target: read from TARGET_REGISTER, written as a shower count to STEP_REGISTER, only while the heater is in
/// manual mode (DhwMode::MANUAL_ECO_ON), with the given rounding policy.
constexpr DeviceReadingRow target_row(RoundingPolicy policy = RoundingPolicy::EXACT) {
  return {.name = "hot_water_target",
          .source = RegisterSource{.fn = TARGET_REGISTER},
          .reading = DeviceReading::HOT_WATER_TARGET,
          .poll = ReadingPoll::EVERY_POLL,
          .feature = DeviceFeature::WATER_HEATER,
          .write = ValueWrite{.target = RegisterWrite{STEP_REGISTER},
                              .guard = WriteGuard{.reading = DeviceReading::HOT_WATER_MODE,
                                                  .test = WriteGuardTest::EQUALS,
                                                  .value = static_cast<uint16_t>(DhwMode::MANUAL_ECO_ON)},
                              .rounding = ValueRounding{.policy = policy}}};
}

/// A flag the heater keeps in a register of its own and that is written there.
constexpr DeviceReadingRow flag_row(const char *name, DeviceReading reading, DeviceRegister fn) {
  return {.name = name,
          .source = RegisterSource{.fn = fn},
          .reading = reading,
          .poll = ReadingPoll::EVERY_POLL,
          .feature = DeviceFeature::WATER_HEATER,
          .write = ValueWrite{RegisterWrite{fn}}};
}

/// The rows of the shape in the order the poll reads them: mode, target, boost, away.
inline constexpr std::array<DeviceReadingRow, 4> READINGS = {
    DeviceReadingRow{.name = "hot_water_mode",
                     .source = RegisterSource{.fn = DeviceRegister::DHW_MODE},
                     .reading = DeviceReading::HOT_WATER_MODE,
                     .poll = ReadingPoll::EVERY_POLL,
                     .feature = DeviceFeature::WATER_HEATER,
                     .write = ValueWrite{RegisterWrite{DeviceRegister::DHW_MODE}}},
    target_row(),
    flag_row("hot_water_boost", DeviceReading::HOT_WATER_BOOST, BOOST_REGISTER),
    flag_row("hot_water_away", DeviceReading::HOT_WATER_AWAY, AWAY_REGISTER),
};

/// Makes the shape the registers and readings of every lookup for as long as it lives.
class SteppedHeaterTables {
 public:
  SteppedHeaterTables() = default;

 private:
  ScopedRegisterTable registers_{REGISTERS};
  ScopedReadingTable readings_{READINGS};
};

}  // namespace test::stepped
