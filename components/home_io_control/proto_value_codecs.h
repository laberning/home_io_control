#pragma once

/// @file proto_value_codecs.h
/// @brief The value encodings of the bytes a device reports or is sent, and the one pure decoder and encoder
/// for them.
/// @ingroup hioc_protocol
///
/// A reading comes from the device as a few bytes (the value bytes of a private register reply, a slice of a data
/// object, the data of an event), and a write sends the same bytes back. ValueCodec names how those bytes map onto
/// a number, so the register table (proto_heating.cpp) and every reading source share one vocabulary, one decoder
/// (decode_value()) and one encoder (encode_value()) instead of each keeping its own. The codecs describe the wire
/// only: the unit and the meaning of the number belong to the entity that shows it. This module is pure: it touches no
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
  SCALED_U16,          ///< Unsigned 16-bit little-endian register value from 0 to the row's `max_value`, shown on a
                       ///< scale of its own: the row's knots (ValueForm::scale) map register values to natural units
                       ///< (a brightness register to percent), linear between two knots.
  TENTHS_S16_BE,       ///< Signed 16-bit tenths of a unit, most significant byte first (a water temperature in
                       ///< degrees). Bytes after the two are ignored. Read only: it is never encoded.
  THOUSANDTHS_U32_BE,  ///< Unsigned 32-bit thousandths of a unit, most significant byte first: a counter in Wh
                       ///< shown in kWh. Read only: it is never encoded.
  ANY_NONZERO,         ///< A flag over the bytes handed in: 1 when any of them is non-zero, 0 when all are zero.
                       ///< Read only: it is never encoded.
  MARKED_TENTHS_S16_BE,  ///< Three bytes: a value marker, then signed 16-bit tenths of a unit, most significant byte
                         ///< first (a temperature entry of a data object, `80 00 77` = 11.9). Any other marker is
                         ///< the no-value pattern, as for MARKED_U16_BE. Read only: it is never encoded.
};

/// @brief The leading byte of a MARKED_U16_BE or MARKED_TENTHS_S16_BE value that carries a number.
constexpr uint8_t VALUE_MARKER_PRESENT = 0x80;

/// @brief Scale of the tenths codecs: the wire value is round(unit * this).
constexpr float VALUE_TENTHS_PER_UNIT = 10.0F;

/// @brief Scale of THOUSANDTHS_U32_BE: the wire value is a thousand times the unit.
constexpr float VALUE_THOUSANDTHS_PER_UNIT = 1000.0F;

/// @brief Raw value of HUNDREDTHS_S16 that reports a sensor fault.
constexpr int16_t VALUE_HUNDREDTHS_FAULT_RAW = -1;

/// @brief The highest value a byte with no bound of its own holds, the `max_value` of an unbounded BOUNDED_BYTE.
constexpr uint16_t VALUE_BYTE_MAX = 0xFF;

/// @brief An inclusive span of values a write may carry, in the natural units of the register (degrees).
struct ValueRange {
  float min;  ///< Lowest value accepted.
  float max;  ///< Highest value accepted.
};

/// @brief One knot of a SCALED_U16 scale: a natural value and the register value it stands for.
struct ValueScaleKnot {
  uint16_t natural;  ///< The value in natural units (a percentage).
  uint16_t raw;      ///< The register value.
};

/// @brief Does a scale have at least two knots that rise strictly in both fields?
/// @details The scaled codec divides by the distance between two knots, so a scale with two equal knots would divide
///          by zero and one that falls would map two values onto one. A scale is checked with a `static_assert`.
/// @param scale The knots of a ValueForm::scale.
/// @return True when there are two or more knots and each one is above the one before in `natural` and in `raw`.
constexpr bool value_scale_rises(std::span<const ValueScaleKnot> scale) {
  if (scale.size() < 2)
    return false;
  for (size_t i = 1; i < scale.size(); ++i) {
    if (scale[i].natural <= scale[i - 1].natural || scale[i].raw <= scale[i - 1].raw)
      return false;
  }
  return true;
}

/// @brief Everything the encoder needs to know about one register's value: its codec and the rule a write
/// must meet.
/// @details A register's row builds one of these; encode_value() judges a value against it, so the bound and
///          the byte layout of a write are stated once, next to the register.
struct ValueForm {
  ValueCodec codec{ValueCodec::NONE};  ///< How the value maps onto the bytes.
  uint16_t max_value{0};               ///< BOUNDED_BYTE, BOUNDED_U16, SCALED_U16: highest value.
  /// TENTHS_U16, TENTHS_U16_OR_ZERO, HUNDREDTHS_S16: the values a write may carry, narrowing the span the bytes
  /// hold (a range wider than the bytes never widens it); without it a write may carry any value the bytes hold.
  // NOLINTBEGIN(readability-redundant-member-init): g++'s -Wmissing-field-initializers wants the member
  // initializer before a designated form may leave the field out.
  std::optional<ValueRange> write_range{};
  /// SCALED_U16: the knots of the scale, ascending in both fields, the first at register value 0 and the last at
  /// `max_value`.
  std::span<const ValueScaleKnot> scale{};
  // NOLINTEND(readability-redundant-member-init)
};

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
/// @param max_value Highest value of BOUNDED_BYTE, BOUNDED_U16 and SCALED_U16; ignored by the other codecs.
/// @return The value, or nullopt when the bytes carry none.
std::optional<float> decode_value(ValueCodec codec, std::span<const uint8_t> bytes, uint16_t max_value = 0);

/// @brief Decode the value bytes of a reading by a register's form: decode_value() with the form's scale as well.
/// @param form Codec, bound and scale of the register.
/// @param bytes The value bytes, starting at the first one.
/// @return The value in natural units (a SCALED_U16 value is unrounded: register value 100 on a 0-10000 scale
///         that has a knot at 33 % = 1000 shows as 3.3), or nullopt when the bytes carry none.
std::optional<float> decode_value(const ValueForm &form, std::span<const uint8_t> bytes);

/// @brief Is `value` a whole number from 0 to `max_value`? NaN and infinity are not.
/// @param value Value to judge.
/// @param max_value Highest whole number accepted.
bool value_is_bounded_integer(float value, uint16_t max_value);

/// @brief Encode a value into the bytes a write sends: the inverse of decode_value().
/// @details A value the register does not take is refused, never rounded into range or truncated: the tenths
///          forms accept a finite value inside the codec's own span and the form's `write_range` and
///          round to the nearest tenth, half away from zero; BINARY takes 0 or 1; the bounded forms and the
///          whole-unit forms take a whole number inside their bound; TENTHS_U16_OR_ZERO also takes exactly 0,
///          the "no value" that cancels. SCALED_U16 takes a value from the first to the last knot of the scale and
///          rounds the register value to the nearest whole one. HUNDREDTHS_S16 never encodes its sensor-fault pattern.
///          NONE, FIRST_BYTE_OF_PAIR (a value with a limit of its own) and the read-only forms TENTHS_S16_BE,
///          THOUSANDTHS_U32_BE and ANY_NONZERO are not writable forms.
/// @param form Codec and the rule of the register.
/// @param value The value in natural units.
/// @param out Receives the value bytes, in the order the register reads them back in.
/// @return The number of bytes written (value_codec_length()), or 0 when the value is refused or `out` is
///         too short.
size_t encode_value(const ValueForm &form, float value, std::span<uint8_t> out);

}  // namespace home_io_control
}  // namespace esphome
