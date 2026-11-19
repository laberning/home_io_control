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
/// Its target temperature is the main parameter of a status reply, on the ordinary relative scale (0..0xC800) over
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
/// EXECUTE with the main parameter MAIN_PARAMETER_KEEP and one of three fixed FP16 values. The heater reports its
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

/// @brief Lower end of the target-temperature scale: main parameter 0 is 20 C.
static constexpr float DHW_TARGET_MIN_C = 20.0F;
/// @brief Upper end of the target-temperature scale: main parameter 0xC800 is 62 C, the highest target the
/// Calypso's display offers.
static constexpr float DHW_TARGET_MAX_C = 62.0F;

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

/// @brief The two features the operating-mode word switches; dhw_operating_feature_wire() holds their wire facts.
enum class DhwOperatingFeature : uint8_t {
  BOOST,  ///< Relaunch.
  AWAY,   ///< Absence (holiday).
};

/// @brief Lowest bit of the boost (relaunch) field of FP16: bits 8-9.
static constexpr uint8_t DHW_FP16_BOOST_SHIFT = 8;
/// @brief Lowest bit of the away (absence) field of FP16: bits 10-11.
static constexpr uint8_t DHW_FP16_AWAY_SHIFT = 10;

/// @brief The wire facts of one DhwOperatingFeature: what FP15, FP16 and the duration registers say about it.
struct DhwOperatingFeatureWire {
  uint16_t capability;               ///< FP15 bit that says the heater supports it (DHW_CAPABILITY_*).
  uint8_t fp16_shift;                ///< Lowest bit of its two-bit FP16 field (0 keep, 1 on, 2 off).
  uint16_t on_word;                  ///< FP16 that starts it and ends the other (DHW_OPERATING_MODE_*_ON).
  DeviceRegister duration_register;  ///< Register that holds its duration.
  uint8_t start_duration;            ///< Duration a start writes before the FP16 EXECUTE.
};

/// @brief The wire facts of `which`: the one place each feature's FP15 bit, FP16 field and duration live.
/// @param which Boost or away.
/// @return Its row.
const DhwOperatingFeatureWire &dhw_operating_feature_wire(DhwOperatingFeature which);

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

}  // namespace home_io_control
}  // namespace esphome
