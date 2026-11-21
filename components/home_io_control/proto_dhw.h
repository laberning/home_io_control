#pragma once

/// @file proto_dhw.h
/// @brief Wire facts of a domestic hot water heater that are not registers: its target scale, its status read and
/// its operating-mode word.
/// @ingroup hioc_protocol
///
/// A domestic hot water heater (node type 51, subtype 0) answers the private register reads a heater answers,
/// `0C 60 00 <index>` in a CMD_WRITE_PRIVATE (0x20): its registers are rows of the register table
/// (proto_heating.h, DeviceRegister::DHW_*), and what the hub keeps of them are readings
/// (proto_device_readings.h). What is here is what a register cannot carry.
///
/// Its target temperature is the main parameter of a status reply, on the ordinary relative scale (0..0xC800) over a
/// range that is a fact of the heater's model: the range of its target row (DeviceReading::HOT_WATER_TARGET in
/// proto_device_readings.h, selected by a capability of the group HOT_WATER_TARGET, ADR 0110). The Calypso's row is
/// DHW_TARGET_MIN_C..DHW_TARGET_MAX_C. Three replies pin it: 54 C reads `A1E8`, 62 C reads `C800`, and AUTO reads
/// `BE7A` = 60.0 C.
///
/// Every reading comes from one Atlantic Calypso Connecte (200 L) whose owner read each register in several display
/// states (issue #198; corpus tests/corpus/captures/exchange/atlantic_calypso_exchange_register_read.yaml and
/// tests/corpus/captures/probe/atlantic_calypso_probe_status_mp_fp.yaml, pinned by corpus_dhw_test.cpp).
///
/// The mode write (`0C 61 00 06 <mode>`) and the target EXECUTE are confirmed by the owner of that heater: its
/// display follows both (issue #198). No reply to either is in the corpus yet.
///
/// Boost and away travel in the operating-mode word, functional parameter 16, not in a register (ADR 0081): one
/// EXECUTE with the main parameter MAIN_PARAMETER_KEEP and one of three fixed FP16 values. The rows of
/// DeviceReading::HOT_WATER_BOOST and HOT_WATER_AWAY (proto_device_readings.cpp) hold the words, the duration
/// registers and the FP15 bits of the Atlantic Calypso. The heater reports its
/// capabilities (FP15) and the current word (FP16) in the status read DHW_STATUS_FPI1 / DHW_STATUS_FPI2 (corpus
/// tests/corpus/captures/probe/atlantic_calypso_probe_status_fp_high.yaml). The registers
/// DeviceRegister::DHW_BOOST_DURATION and DHW_AWAY_DURATION hold the boost and away durations; a write of a duration
/// alone starts nothing. The three FP16 values start and end a boost and the holiday mode on an Atlantic Calypso
/// Connecté (issue #198).

#include "proto_constants.h"
#include "proto_heating.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

// === Durations a start writes ===

/// @brief Duration a boost start writes to DeviceRegister::DHW_BOOST_DURATION: the value read during a boost started
/// on the display, the only non-zero value seen.
static constexpr uint8_t DHW_BOOST_START_DURATION = 1;
/// @brief Duration an away start writes to DeviceRegister::DHW_AWAY_DURATION: 255, which the heater itself holds for a
/// holiday with no end date (issue #198), as Home Assistant's away mode (no end date) means.
static constexpr uint8_t DHW_AWAY_UNTIL_TURNED_OFF = 255;

// === Target temperature ===

/// @brief Lower end of the Atlantic Calypso's target scale (the row with DeviceFeature::HOT_WATER_TARGET_20_62):
/// main parameter 0 is 20 C.
static constexpr float DHW_TARGET_MIN_C = 20.0F;
/// @brief Upper end of the Atlantic Calypso's target scale: main parameter 0xC800 is 62 C, the highest target its
/// display offers.
static constexpr float DHW_TARGET_MAX_C = 62.0F;
/// @brief Upper end of the Atlantic Explorer V4's target scale (the row with DeviceFeature::HOT_WATER_TARGET_20_70):
/// main parameter 0xC800 is 70 C. Its lower end is the Calypso's: 7C00 reads 51 C, 8C00 55 C and 9C00 59 C on its
/// display (pull request #246).
static constexpr float DHW_EXPLORER_V4_TARGET_MAX_C = 70.0F;
/// @brief Step of the Atlantic Explorer V4's target: it answers a target between two whole degrees with result code
/// 10 and rounds it, so the hub sends whole degrees only.
static constexpr float DHW_EXPLORER_V4_TARGET_STEP_C = 1.0F;
/// @brief Highest count of a Sauter Guelma's manual shower register (DeviceRegister::DHW_MANUAL_SHOWERS): 4 showers.
/// It reports its range, 2 to 4, in register 0x42 (corpus `sauter_guelma_exchange_register_read`).
static constexpr uint16_t DHW_MANUAL_SHOWERS_MAX = 4;
/// @brief A Sauter Guelma's manual shower counts and the temperatures its display shows for them: 2, 3 and 4 showers
/// for 50, 60 and 70 C, read back in register 0x49 after each was set. These are its target's only steps.
inline constexpr ValueScaleKnot DHW_MANUAL_SHOWER_KNOTS[] = {{50, 2}, {60, 3}, {70, DHW_MANUAL_SHOWERS_MAX}};
static_assert(value_scale_rises(DHW_MANUAL_SHOWER_KNOTS));

// === Operating mode (functional parameters 15 and 16) ===

/// @brief FP16 that starts a boost and ends away. Bit 14 is set as the heater reports it.
static constexpr uint16_t DHW_OPERATING_MODE_BOOST_ON = 0x4900;
/// @brief FP16 that ends a boost and ends away: both fields off.
static constexpr uint16_t DHW_OPERATING_MODE_OFF = 0x4A00;
/// @brief FP16 that starts away (holiday) and ends a boost.
static constexpr uint16_t DHW_OPERATING_MODE_AWAY_ON = 0x4600;
/// @brief Status-read FPI1 that asks for no FP1-FP8 (the heater holds none of them).
static constexpr uint8_t DHW_STATUS_FPI1 = 0x00;
/// @brief Functional parameter that carries the operating-mode capabilities (DHW_CAPABILITY_* bits).
static constexpr uint8_t DHW_FP_CAPABILITIES = 15;
/// @brief Functional parameter that carries the operating-mode word (DHW_OPERATING_MODE_*).
static constexpr uint8_t DHW_FP_OPERATING_MODE = 16;
/// @brief Functional parameter 10: no decision uses it; the status read asks for it so the read is exactly the
/// one the heater has been seen answering (FPI2 `43`, issue #198). No reading is that parameter, so its value is
/// skipped.
static constexpr uint8_t DHW_FP_ALSO_ASKED = 10;
/// @brief Status-read FPI2 (`43`): FP10, FP15 and FP16. The capability and operating-mode words come with the
/// main parameter in one exchange; FP10 keeps the read identical to the one the heater answered.
static constexpr uint8_t DHW_STATUS_FPI2 =
    fp_bitmap_bit(DHW_FP_ALSO_ASKED) | fp_bitmap_bit(DHW_FP_CAPABILITIES) | fp_bitmap_bit(DHW_FP_OPERATING_MODE);
/// @brief FP15 bit 2: the heater supports away (absence).
static constexpr uint16_t DHW_CAPABILITY_AWAY = 1U << 2U;
/// @brief FP15 bit 3: the heater supports boost (relaunch).
static constexpr uint16_t DHW_CAPABILITY_BOOST = 1U << 3U;

/// @brief Lowest bit of the boost (relaunch) field of FP16: bits 8-9.
static constexpr uint8_t DHW_FP16_BOOST_SHIFT = 8;
/// @brief Lowest bit of the away (absence) field of FP16: bits 10-11.
static constexpr uint8_t DHW_FP16_AWAY_SHIFT = 10;
/// @brief Boost field of FP16 during a boost on an Atlantic Explorer V4: it reports 4A00 then and 4900 at rest, the
/// Calypso's words the other way round (pull request #246).
static constexpr uint8_t DHW_EXPLORER_V4_BOOST_FIELD = 2;
/// @brief Main parameter an Atlantic Explorer V4 reports while away: its FP16 stays 4900, so this is the only sign
/// (pull request #246).
static constexpr uint16_t DHW_EXPLORER_V4_AWAY_MAIN_PARAMETER = 0x0000;

// === Value shapes ===

/// @brief Operating mode as register DeviceRegister::DHW_MODE holds it.
enum class DhwMode : uint8_t {
  AUTO = 0,            ///< The heater's automatic mode (its own schedule and target).
  MANUAL_ECO_ON = 1,   ///< Manual mode with ECO on (heat pump only).
  MANUAL_ECO_OFF = 2,  ///< Manual mode with ECO off (the electric element may help).
};

/// @brief What heats the water, as a short label, from the two heating readings.
/// @details "heat pump", "electric backup", "heat pump and electric backup" or "idle" (neither runs). Both readings
///          are slices of the same register, so a label needs both.
/// @param heat_pump DeviceReading::HOT_WATER_HEAT_PUMP as a whole number (0, 1 or -1 for not read).
/// @param element DeviceReading::HOT_WATER_ELECTRIC_BACKUP as a whole number (0, 1 or -1 for not read).
/// @return The label, or nullptr while either reading is missing.
const char *dhw_heating_source_name(int heat_pump, int element);

/// @brief The name of a mode value of DeviceRegister::DHW_MODE: "auto", "manual eco on" or "manual eco off".
/// @param value The register's value.
/// @return The name, or nullptr for a value the heater has not been seen reporting.
const char *dhw_mode_name(uint8_t value);

/// @brief The name of a mode of an electric storage water heater's mode register, which has no mode with ECO on.
/// @details "auto" for DhwMode::AUTO and "manual electric" for DhwMode::MANUAL_ECO_OFF, which is what a Sauter Guelma
///          holds in manual mode, where only its element heats. The card offers the modes this names.
/// @param value Raw register byte.
/// @return Null-terminated name; nullptr for DhwMode::MANUAL_ECO_ON and any other value.
const char *dhw_electric_mode_name(uint8_t value);

}  // namespace home_io_control
}  // namespace esphome
