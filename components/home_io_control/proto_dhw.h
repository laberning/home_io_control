#pragma once

/// @file proto_dhw.h
/// @brief Pure codec for a domestic hot water heater: its family-0x00 registers and its target temperature.
/// @ingroup hioc_protocol
///
/// A domestic hot water heater (node type 51, subtype 0) answers the private register reads a heater
/// answers, `0C 60 00 <index>` in a CMD_WRITE_PRIVATE (0x20), with a CMD_WRITE_PRIVATE_ACK (0x21) that
/// echoes the four header bytes and then the value. Its registers sit in family 0x00 (the high byte of the
/// register number) and their multi-byte values are **big-endian**, where the radiator family 0x01 of
/// proto_heating.h is little-endian; so this is a codec of its own, not a flag on the heater's. An index the
/// heater does not hold is answered with no value bytes.
///
/// Its target temperature is not a register: it is the main parameter of a status reply, on the ordinary
/// relative scale (0..0xC800) over DHW_TARGET_MIN_C..DHW_TARGET_MAX_C. Three replies pin it: 54 C reads
/// `A1E8`, 62 C reads `C800`, and AUTO reads `BE7A` = 60.0 C.
///
/// Every reading here comes from one Atlantic Calypso Connecte (200 L) whose owner read each register in
/// several display states (issue #198; corpus
/// tests/corpus/captures/exchange/atlantic_calypso_exchange_register_read.yaml and
/// tests/corpus/captures/probe/atlantic_calypso_probe_status_mp_fp.yaml, pinned by corpus_dhw_test.cpp).
///
/// The mode write (`0C 61 00 06 <mode>`) and the target EXECUTE are confirmed by the owner of that heater:
/// its display follows both (issue #198). No reply to either is in the corpus yet.
///
/// Boost and away travel in the operating-mode word, functional parameter 16, not in a register (ADR 0081):
/// one EXECUTE with the main parameter MAIN_PARAMETER_KEEP and one of three fixed FP16 values. The
/// heater reports its capabilities (FP15) and the current word (FP16) in the status read
/// DHW_STATUS_FPI1 / DHW_STATUS_FPI2 (corpus
/// tests/corpus/captures/probe/atlantic_calypso_probe_status_fp_high.yaml). Registers 0x0000 and 0x0005 hold
/// the boost and away durations; a write of a duration alone starts nothing.
/// \todo Experimental: confirm on a real water heater that the three FP16 values start and end a boost and
/// the holiday mode (issue #198).

#include "proto_constants.h"
#include "proto_frame.h"
#include "proto_heating.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

// === Registers ===

/// @brief High byte of every register of a domestic hot water heater (family 0x00).
static constexpr uint8_t DHW_REGISTER_HIGH_BYTE = 0x00;

/// @brief Register 0x0000: boost duration, one byte. It read 1 during a boost started on the display and 0
/// before and after it (issue #198). A duration is not the state: the state is FP16 (DhwOperatingStatus).
static constexpr uint8_t DHW_REG_BOOST_DURATION = 0x00;
/// @brief Register 0x0005: away (holiday) duration, one byte: the days the holiday still runs. It read 0 at
/// rest and with a holiday scheduled for later, 4 and 2 four and two days before the end, and 255 for a
/// holiday set on the display with no end date (issue #198).
static constexpr uint8_t DHW_REG_AWAY_DURATION = 0x05;
/// @brief Duration a boost start writes to register 0x0000: the value read during a boost started on the
/// display, the only non-zero value seen.
static constexpr uint8_t DHW_BOOST_START_DURATION = 1;
/// @brief Duration an away start writes to register 0x0005: 255, which the heater itself holds for a holiday
/// with no end date (issue #198), as Home Assistant's away mode (no end date) means.
static constexpr uint8_t DHW_AWAY_UNTIL_TURNED_OFF = 255;
/// @brief Register 0x0004: energy consumed since the heater was installed, 32-bit big-endian, Wh. Matches the
/// display's "since start" total to the kWh, and rose while the heat pump ran.
static constexpr uint8_t DHW_REG_ENERGY = 0x04;
/// @brief Register 0x0006: operating mode (DhwMode), one byte. Followed the display in every run.
static constexpr uint8_t DHW_REG_MODE = 0x06;
/// @brief Register 0x0007: one ASCII letter naming the brand (`41` = 'A' on the Calypso).
static constexpr uint8_t DHW_REG_BRAND = 0x07;
/// @brief Register 0x0017: water temperature, signed 16-bit big-endian tenths of a degree in the first two
/// value bytes. Four more bytes follow that never changed; they are not decoded.
static constexpr uint8_t DHW_REG_WATER_TEMPERATURE = 0x17;
/// @brief Register 0x0021: what is heating, eight bytes (DhwHeatingState).
static constexpr uint8_t DHW_REG_HEATING = 0x21;

// === Target temperature ===

/// @brief Lower end of the target-temperature scale: main parameter 0 is 20 C.
static constexpr float DHW_TARGET_MIN_C = 20.0F;
/// @brief Upper end of the target-temperature scale: main parameter 0xC800 is 62 C, the highest target the
/// Calypso's display offers.
static constexpr float DHW_TARGET_MAX_C = 62.0F;
/// @brief Offset of the main parameter (MSB) in the data of a status reply (CMD_PRIVATE_RESP): the target
/// slot of a cover's reply. The two bytes after it read `00 00` in every reply seen.
static constexpr uint8_t DHW_STATUS_MAIN_PARAMETER_OFFSET = 2;

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
/// one the heater has been seen answering (FPI2 `43`, issue #198). The decoder skips its value.
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

/// @brief The wire facts of one DhwOperatingFeature: what FP15, FP16 and the duration registers say about it.
struct DhwOperatingFeatureWire {
  uint16_t capability;        ///< FP15 bit that says the heater supports it (DHW_CAPABILITY_*).
  uint8_t fp16_shift;         ///< Lowest bit of its two-bit FP16 field (0 keep, 1 on, 2 off).
  uint16_t on_word;           ///< FP16 that starts it and ends the other (DHW_OPERATING_MODE_*_ON).
  uint8_t duration_register;  ///< Register that holds its duration (DHW_REG_*_DURATION).
  uint8_t start_duration;     ///< Duration a start writes before the FP16 EXECUTE.
};

/// @brief The wire facts of `which`: the one place each feature's FP15 bit, FP16 field and duration live.
/// @param which Boost or away.
/// @return Its row.
const DhwOperatingFeatureWire &dhw_operating_feature_wire(DhwOperatingFeature which);

/// @brief The operating-mode part of the status read DHW_STATUS_FPI1 / DHW_STATUS_FPI2 (its main parameter,
/// the target, is decode_dhw_status_target()'s).
struct DhwOperatingStatus {
  uint16_t capabilities{0};  ///< FP15: DHW_CAPABILITY_* bits.
  bool boost{false};         ///< FP16 relaunch field (bits 8-9) is 1: a boost runs.
  bool away{false};          ///< FP16 absence field (bits 10-11) is 1: the holiday mode is on.
};

// === Value shapes ===

/// @brief Operating mode as register 0x0006 holds it.
enum class DhwMode : uint8_t {
  AUTO = 0,            ///< The heater's automatic mode (its own schedule and target).
  MANUAL_ECO_ON = 1,   ///< Manual mode with ECO on (heat pump only).
  MANUAL_ECO_OFF = 2,  ///< Manual mode with ECO off (the electric element may help).
};

/// @brief What register 0x0021 says is heating.
struct DhwHeatingState {
  bool heating{false};    ///< Byte 0: the heater is heating at all (heat pump, element or both).
  bool element{false};    ///< Byte 2: the electric element is on (set during boost only).
  bool heat_pump{false};  ///< Bytes 3 and 4: the heat pump runs (both were always set together).
};

/// @brief What the hub has read from a domestic hot water heater.
///
/// Every field starts as "not read" and is set only from a decoded reply, so a sensor never shows a value the
/// heater did not report. Mirrors HeatingReadings for the radiator family.
struct DhwReadings {
  float water_c{NAN};          ///< Water temperature (register 0x0017); NaN until read.
  float target_c{NAN};         ///< Target temperature (status reply main parameter); NaN until read or when the
                               ///< main parameter is not a temperature.
  int16_t mode{-1};            ///< DhwMode value; -1 until read.
  int8_t heating{-1};          ///< DhwHeatingState::heating as 0/1; -1 until read.
  int8_t element{-1};          ///< DhwHeatingState::element as 0/1; -1 until read.
  int8_t heat_pump{-1};        ///< DhwHeatingState::heat_pump as 0/1; -1 until read.
  int32_t capabilities{-1};    ///< FP15 operating-mode capabilities (DHW_CAPABILITY_*); -1 until read.
  int8_t boost{-1};            ///< Boost on (1) or off (0) as FP16 reports it; -1 until read.
  int8_t away{-1};             ///< Away on (1) or off (0) as FP16 reports it; -1 until read.
  int16_t boost_duration{-1};  ///< Register 0x0000; -1 until read. Logged, never a state.
  int16_t away_duration{-1};   ///< Register 0x0005; -1 until read. Logged, never a state.
  int64_t energy_wh{-1};       ///< Energy since installation in Wh; -1 until read.
  int16_t brand{-1};           ///< Brand letter (register 0x0007); -1 until read.
};

// === Requests ===

/// @brief Encode a read of one family-0x00 register: `0C 60 00 <index>`.
/// @param index Low byte of the register number.
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return Always 4.
size_t encode_dhw_register_read(uint8_t index, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

/// @brief Encode a write of the operating mode: `0C 61 00 06 <mode>`.
/// @details Only the three values the heater has reported (DhwMode) are encoded; anything else is refused, so
///          a byte the heater has never shown is never written to it.
/// @param mode Mode value.
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return 5, or 0 for a value other than 0, 1 or 2.
size_t encode_dhw_mode_write(uint8_t mode, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

/// @brief Encode the duration write a start of `which` sends before its FP16 EXECUTE: `0C 61 00 00 01` for
/// boost (DHW_BOOST_START_DURATION), `0C 61 00 05 FF` for away (DHW_AWAY_UNTIL_TURNED_OFF).
/// @details The heater has been seen to ignore the boost duration on its own; the write is part of the
///          sequence so its acknowledgement shows what the heater keeps.
/// \todo Experimental: confirm on the heater what these durations do (issue #198).
/// @param which Boost or away.
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return Always 5.
size_t encode_dhw_duration_write(DhwOperatingFeature which, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

/// @brief Encode a target temperature as a main parameter over DHW_TARGET_MIN_C..DHW_TARGET_MAX_C.
/// @param celsius Target; must be finite and within the range (nothing outside it is ever sent).
/// @param[out] main_parameter The encoded value; untouched when false is returned.
/// @return false for a non-finite or out-of-range target.
bool encode_dhw_target(float celsius, uint16_t &main_parameter);

// === Replies ===

/// @brief The value bytes of a reply to a family-0x00 register read.
/// @details Accepts a CMD_WRITE_PRIVATE_ACK whose data starts with `0C 60 00 <index>` (or `0C 61 00 <index>`,
///          the ack of a write). The value may be empty: the heater answers an index it does not hold so.
/// @param reply The heater's reply.
/// @param index The register the request addressed.
/// @param[out] value First value byte (inside `reply`); untouched when false is returned.
/// @param[out] len Number of value bytes; untouched when false is returned.
/// @return false for another command, another register or another family.
bool decode_dhw_register_reply(const IoFrame &reply, uint8_t index, const uint8_t *&value, size_t &len);

/// @brief Water temperature from the value of register 0x0017: signed 16-bit big-endian tenths.
/// @param value Value bytes.
/// @param len Number of value bytes; at least 2.
/// @param[out] celsius Temperature; untouched when false is returned.
/// @return false for a value shorter than two bytes.
bool decode_dhw_water_temperature(const uint8_t *value, size_t len, float &celsius);

/// @brief Operating mode from the value of register 0x0006.
/// @param value Value bytes.
/// @param len Number of value bytes; at least 1.
/// @param[out] mode The mode; untouched when false is returned.
/// @return false for an empty value or a byte other than 0, 1 or 2.
bool decode_dhw_mode(const uint8_t *value, size_t len, DhwMode &mode);

/// @brief What is heating, from the value of register 0x0021.
/// @param value Value bytes.
/// @param len Number of value bytes; at least 5.
/// @param[out] state The decoded state; untouched when false is returned.
/// @return false for a value shorter than five bytes.
bool decode_dhw_heating_state(const uint8_t *value, size_t len, DhwHeatingState &state);

/// @brief Energy since installation from the value of register 0x0004: unsigned 32-bit big-endian, Wh.
/// @param value Value bytes.
/// @param len Number of value bytes; at least 4.
/// @param[out] wh Energy in Wh; untouched when false is returned.
/// @return false for a value shorter than four bytes.
bool decode_dhw_energy_wh(const uint8_t *value, size_t len, uint32_t &wh);

/// @brief A duration from the value of register 0x0000 or 0x0005: one byte.
/// @param value Value bytes.
/// @param len Number of value bytes; at least 1.
/// @param[out] duration The byte; untouched when false is returned.
/// @return false for an empty value.
bool decode_dhw_duration(const uint8_t *value, size_t len, uint8_t &duration);

/// @brief Target temperature from a main parameter.
/// @param main_parameter The main parameter as the status reply carries it.
/// @param[out] celsius Target, rounded to a tenth of a degree; untouched when false is returned.
/// @return false for a main parameter beyond 0xC800 (an alias such as 0xD8xx is not a temperature).
bool decode_dhw_target(uint16_t main_parameter, float &celsius);

/// @brief Target temperature from a status reply (CMD_PRIVATE_RESP) of a domestic hot water heater.
/// @param reply The heater's reply to a status read.
/// @param[out] celsius Target; untouched when false is returned.
/// @return false for another command, a reply too short to carry the main parameter, or a main parameter
///         that is not a temperature.
bool decode_dhw_status_target(const IoFrame &reply, float &celsius);

/// @brief Capabilities and operating-mode state from the reply to the status read
/// DHW_STATUS_FPI1 / DHW_STATUS_FPI2.
/// @details Layout of the data: `[status][b1][MP 2][FPI1][FPI2][one 2-byte value per FPI2 bit, bit 7 first]`.
///          The echo must carry FPI1 `00` and both the FP15 and FP16 bits; other bits (the heater echoes FP10
///          when asked for `43`) are skipped. The main parameter is left to decode_dhw_status_target().
/// @param reply The heater's reply to the status read.
/// @param[out] status The decoded fields; untouched when false is returned.
/// @return false for another command, a reply too short for the values its echo announces, an FPI1 echo other
///         than `00`, or an FPI2 echo without FP15 or FP16.
bool decode_dhw_operating_status(const IoFrame &reply, DhwOperatingStatus &status);

}  // namespace home_io_control
}  // namespace esphome
