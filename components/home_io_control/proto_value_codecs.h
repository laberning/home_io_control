#pragma once

/// @file proto_value_codecs.h
/// @brief The value encodings of the bytes a device reports, and the one pure decoder for them.
/// @ingroup hioc_protocol
///
/// A reading comes from the device as a few bytes (the value bytes of a private register reply today;
/// the slice of a data object or a one-frame read next). ValueCodec names how those bytes map onto a
/// number, so the heater register table (proto_heating.cpp) and every later reading source share one
/// vocabulary and one decoder instead of each keeping its own. The codecs describe the wire only: the unit
/// and the meaning of the number belong to the entity that shows it. This module is pure: it touches no
/// device record and no radio.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace esphome {
namespace home_io_control {

/// @brief How a register's value maps onto the bytes after the header of its reply.
enum class ValueCodec : uint8_t {
  NONE,                ///< No typed value: logged raw, never decoded.
  TENTHS_U16,          ///< Unsigned 16-bit little-endian tenths of a unit (a temperature in degrees).
  TENTHS_U16_OR_ZERO,  ///< TENTHS_U16, and a write may also send 0 (`00 00`): "no value", which cancels.
  HUNDREDTHS_S16,      ///< Signed 16-bit little-endian hundredths of a unit; -1 is a sensor fault (decodes as NaN).
  BINARY,              ///< One byte, 0 or 1.
  BOUNDED_BYTE,        ///< One byte from 0 to the row's `max_value`: an enumeration (a mode, a state), a percentage or
                       ///< a count; 255 for a byte with no bound (minutes left of a run).
  BOUNDED_U16,         ///< Unsigned 16-bit little-endian integer from 0 to the row's `max_value`.
  U16_BE,              ///< Unsigned 16-bit whole units, most significant byte first: the one register family whose
                       ///< 16-bit value is not little-endian (an energy total in kilowatt-hours).
  MARKED_U16_BE,       ///< Three bytes: a value marker, then an unsigned 16-bit whole number, most significant byte
                       ///< first (a counter of a data object, `80 0C BE` = 3262). Any other marker is the device's
                       ///< no-value pattern (`81 FF FF`), which decodes as no value.
  FIRST_BYTE_OF_PAIR,  ///< Two bytes, a value then a limit of its own; the first byte is the value (a duration in
                       ///< minutes then the panel's maximum).
};

/// @brief The leading byte of a MARKED_U16_BE value that carries a number.
constexpr uint8_t VALUE_MARKER_PRESENT = 0x80;

/// @brief Scale of the tenths codecs: the wire value is round(unit * this).
constexpr float VALUE_TENTHS_PER_UNIT = 10.0F;

/// @brief Raw value of HUNDREDTHS_S16 that reports a sensor fault.
constexpr int16_t VALUE_HUNDREDTHS_FAULT_RAW = -1;

/// @brief The highest value a byte with no bound of its own holds, the `max_value` of an unbounded BOUNDED_BYTE.
constexpr uint16_t VALUE_BYTE_MAX = 0xFF;

/// @brief Number of value bytes a codec reads.
/// @param codec Encoding.
/// @return The byte count; 0 for NONE.
size_t value_codec_length(ValueCodec codec);

/// @brief Decode the value bytes of a reading.
/// @details The value is returned in natural units: tenths and hundredths divided down, whole units as they are,
///          and the raw number for bytes and flags. A flag above 1 and a bounded byte or word above
///          `max_value` are "no value", as is a value that is shorter than the codec reads. A raw -1 on
///          HUNDREDTHS_S16 is a sensor fault: it decodes as NaN, with a value returned, because the device did answer.
/// @param codec Encoding of the bytes.
/// @param bytes The value bytes, starting at the first one; trailing bytes the codec does not read are ignored.
/// @param max_value Highest value of BOUNDED_BYTE and BOUNDED_U16; ignored by the other codecs.
/// @return The value, or nullopt when the bytes carry none.
std::optional<float> decode_value(ValueCodec codec, std::span<const uint8_t> bytes, uint16_t max_value = 0);

}  // namespace home_io_control
}  // namespace esphome
