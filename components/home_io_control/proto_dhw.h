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
/// \todo The mode write (`0C 61 00 06 <mode>`) and the target EXECUTE are unconfirmed on hardware: no
///       reply to either has been captured yet. Confirm that the heater's display follows both.

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

/// @brief Register 0x0000: boost. 1 while the owner's one-day boost ran, 0 before and after (one state each).
static constexpr uint8_t DHW_REG_BOOST = 0x00;
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
  float water_c{NAN};     ///< Water temperature (register 0x0017); NaN until read.
  float target_c{NAN};    ///< Target temperature (status reply main parameter); NaN until read or when the
                          ///< main parameter is not a temperature.
  int16_t mode{-1};       ///< DhwMode value; -1 until read.
  int8_t heating{-1};     ///< DhwHeatingState::heating as 0/1; -1 until read.
  int8_t element{-1};     ///< DhwHeatingState::element as 0/1; -1 until read.
  int8_t heat_pump{-1};   ///< DhwHeatingState::heat_pump as 0/1; -1 until read.
  int8_t boost{-1};       ///< Boost on (1) or off (0); -1 until read.
  int64_t energy_wh{-1};  ///< Energy since installation in Wh; -1 until read.
  int16_t brand{-1};      ///< Brand letter (register 0x0007); -1 until read.
};

// === Requests ===

/// @brief Encode a read of one family-0x00 register: `0C 60 00 <index>`.
/// @param index Low byte of the register number.
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return Always 4.
size_t encode_dhw_register_read(uint8_t index, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

/// @brief Encode a write of the operating mode: `0C 61 00 06 <mode>`, the only family-0x00 write.
/// @details Only the three values the heater has reported (DhwMode) are encoded; anything else is refused, so
///          a byte the heater has never shown is never written to it.
/// @param mode Mode value.
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return 5, or 0 for a value other than 0, 1 or 2.
size_t encode_dhw_mode_write(uint8_t mode, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

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

/// @brief Boost from the value of register 0x0000: any non-zero byte is "on".
/// @param value Value bytes.
/// @param len Number of value bytes; at least 1.
/// @param[out] on Boost on; untouched when false is returned.
/// @return false for an empty value.
bool decode_dhw_boost(const uint8_t *value, size_t len, bool &on);

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

}  // namespace home_io_control
}  // namespace esphome
