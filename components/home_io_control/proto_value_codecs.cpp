/// @file proto_value_codecs.cpp
/// @brief The value encodings of the bytes a device reports or is sent: the pure decoder and encoder for them.
/// @ingroup hioc_protocol

#include "proto_value_codecs.h"

#include "proto_codecs.h"
#include "proto_sizes.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace esphome {
namespace home_io_control {

namespace {

/// Scale of HUNDREDTHS_S16: signed hundredths of a unit.
constexpr float HUNDREDTHS_PER_UNIT = 100.0F;

/// The widest hundredths value a signed 16-bit word holds, in units: 327.67.
constexpr float HUNDREDTHS_S16_MAX = 32767.0F / HUNDREDTHS_PER_UNIT;

/// Mask for the low byte of a 16-bit value.
constexpr uint16_t LOW_BYTE_MASK = 0xFF;

/// The widest tenths value a 16-bit word holds, in units: 6553.5.
constexpr float TENTHS_U16_MAX = 65535.0F / VALUE_TENTHS_PER_UNIT;

/// @return The 16-bit little-endian word at the start of `bytes` (two bytes must be there).
uint16_t little_endian_u16(std::span<const uint8_t> bytes) {
  return static_cast<uint16_t>(bytes[0] | (bytes[1] << BITS_PER_BYTE));
}

/// @return The 32-bit big-endian word at the start of `bytes` (four bytes must be there).
uint32_t big_endian_u32(std::span<const uint8_t> bytes) {
  return (static_cast<uint32_t>(bytes[0]) << (3U * BITS_PER_BYTE)) |
         (static_cast<uint32_t>(bytes[1]) << (2U * BITS_PER_BYTE)) |
         (static_cast<uint32_t>(bytes[2]) << BITS_PER_BYTE) | static_cast<uint32_t>(bytes[3]);
}

/// Write `raw` little-endian into the first two bytes of `out`.
void put_little_endian_u16(uint16_t raw, std::span<uint8_t> out) {
  out[0] = static_cast<uint8_t>(raw & LOW_BYTE_MASK);
  out[1] = static_cast<uint8_t>(raw >> BITS_PER_BYTE);
}

/// @return Whether `value` is finite and inside the span the codec's bytes hold (`codec_span`) and, where the form
/// has one, its write range: the range narrows the span and never widens it.
bool within_write_range(const ValueForm &form, float value, ValueRange codec_span) {
  if (!std::isfinite(value) || value < codec_span.min || value > codec_span.max)
    return false;
  return !form.write_range || (value >= form.write_range->min && value <= form.write_range->max);
}

/// Encode a value of a tenths codec: refused outside the range, otherwise the nearest tenth.
size_t encode_tenths(const ValueForm &form, float value, std::span<uint8_t> out) {
  if (!within_write_range(form, value, {0.0F, TENTHS_U16_MAX}))
    return 0;
  // Multiply in float (not double): the float32 product of e.g. 20.55f * 10.0f is exactly 205.5f, and
  // std::round() takes that half away from zero to 206 (0x00CE). Truncating or rounding toward even would both be
  // wrong here. This exactness relies on FLT_EVAL_METHOD == 0 (operands evaluated at their own single precision,
  // no hidden widening to double), which holds on x86-64 SSE and on Xtensa; the heater codec test pins 20.55,
  // which is one ULP from flipping to 0xCD under any excess-precision evaluation.
  put_little_endian_u16(static_cast<uint16_t>(std::round(value * VALUE_TENTHS_PER_UNIT)), out);
  return 2;
}

/// @return The segment of the scale that holds `at` (compared by `field`): the knot below and the knot above.
template<typename Field>
std::pair<ValueScaleKnot, ValueScaleKnot> scale_segment(std::span<const ValueScaleKnot> scale, float at, Field field) {
  for (size_t i = 1; i < scale.size(); ++i) {
    if (at <= static_cast<float>(field(scale[i])))
      return {scale[i - 1], scale[i]};
  }
  return {scale[scale.size() - 2], scale.back()};
}

/// The natural value of a register value on the scale, unrounded.
float scaled_natural_from_raw(std::span<const ValueScaleKnot> scale, uint16_t raw) {
  const auto [lo, hi] = scale_segment(scale, static_cast<float>(raw), [](const ValueScaleKnot &k) { return k.raw; });
  const float slope = static_cast<float>(hi.natural - lo.natural) / static_cast<float>(hi.raw - lo.raw);
  return static_cast<float>(lo.natural) + (static_cast<float>(raw - lo.raw) * slope);
}

/// The register value of a natural value on the scale, rounded to the nearest whole one.
uint16_t scaled_raw_from_natural(std::span<const ValueScaleKnot> scale, float natural) {
  const auto [lo, hi] = scale_segment(scale, natural, [](const ValueScaleKnot &k) { return k.natural; });
  const float steps = (natural - static_cast<float>(lo.natural)) * static_cast<float>(hi.raw - lo.raw) /
                      static_cast<float>(hi.natural - lo.natural);
  return static_cast<uint16_t>(static_cast<float>(lo.raw) + std::round(steps));
}

}  // namespace

bool value_is_bounded_integer(float value, uint16_t max_value) {
  return std::isfinite(value) && value >= 0.0F && value <= static_cast<float>(max_value) && value == std::floor(value);
}

size_t value_codec_length(ValueCodec codec) {
  switch (codec) {
    case ValueCodec::NONE:
      return 0;
    case ValueCodec::TENTHS_U16:
    case ValueCodec::TENTHS_U16_OR_ZERO:
    case ValueCodec::HUNDREDTHS_S16:
    case ValueCodec::BOUNDED_U16:
    case ValueCodec::SCALED_U16:
    case ValueCodec::U16_BE:
    case ValueCodec::FIRST_BYTE_OF_PAIR:
    case ValueCodec::TENTHS_S16_BE:
      return 2;
    case ValueCodec::MARKED_U16_BE:
    case ValueCodec::MARKED_TENTHS_S16_BE:
      return 3;
    case ValueCodec::THOUSANDTHS_U32_BE:
      return 4;
    case ValueCodec::BINARY:
    case ValueCodec::BOUNDED_BYTE:
    case ValueCodec::ANY_NONZERO:
      return 1;
  }
  return 0;
}

std::optional<float> decode_value(ValueCodec codec, std::span<const uint8_t> bytes, uint16_t max_value) {
  return decode_value(ValueForm{.codec = codec, .max_value = max_value}, bytes);
}

std::optional<float> decode_value(const ValueForm &form, std::span<const uint8_t> bytes) {
  const ValueCodec codec = form.codec;
  const uint16_t max_value = form.max_value;
  if (codec == ValueCodec::NONE || bytes.size() < value_codec_length(codec))
    return std::nullopt;

  switch (codec) {
    case ValueCodec::TENTHS_U16:
    case ValueCodec::TENTHS_U16_OR_ZERO:
      return static_cast<float>(little_endian_u16(bytes)) / VALUE_TENTHS_PER_UNIT;

    case ValueCodec::HUNDREDTHS_S16: {
      const auto signed_raw = static_cast<int16_t>(little_endian_u16(bytes));
      return signed_raw == VALUE_HUNDREDTHS_FAULT_RAW ? NAN : static_cast<float>(signed_raw) / HUNDREDTHS_PER_UNIT;
    }

    case ValueCodec::BINARY:
      return bytes[0] > 1 ? std::nullopt : std::optional<float>{static_cast<float>(bytes[0])};

    case ValueCodec::BOUNDED_BYTE:
      return bytes[0] > max_value ? std::nullopt : std::optional<float>{static_cast<float>(bytes[0])};

    case ValueCodec::BOUNDED_U16: {
      const uint16_t raw = little_endian_u16(bytes);
      return raw > max_value ? std::nullopt : std::optional<float>{static_cast<float>(raw)};
    }

    case ValueCodec::SCALED_U16: {
      const uint16_t raw = little_endian_u16(bytes);
      if (raw > max_value || form.scale.size() < 2)
        return std::nullopt;
      return scaled_natural_from_raw(form.scale, raw);
    }

    case ValueCodec::U16_BE:
      // Most significant byte first, unlike every little-endian word above.
      return static_cast<float>(read_u16_be(bytes.data()));

    case ValueCodec::MARKED_U16_BE:
      // Only the present-marker carries a number; the other marker means the device holds no value.
      if (bytes[0] != VALUE_MARKER_PRESENT)
        return std::nullopt;
      return static_cast<float>(read_u16_be(bytes.data() + 1));

    case ValueCodec::FIRST_BYTE_OF_PAIR:
      // The second byte (the panel's maximum) is not read.
      return static_cast<float>(bytes[0]);

    case ValueCodec::TENTHS_S16_BE:
      return static_cast<float>(static_cast<int16_t>(read_u16_be(bytes.data()))) / VALUE_TENTHS_PER_UNIT;

    case ValueCodec::MARKED_TENTHS_S16_BE:
      // The marker as for MARKED_U16_BE, then a signed word of tenths.
      if (bytes[0] != VALUE_MARKER_PRESENT)
        return std::nullopt;
      return static_cast<float>(static_cast<int16_t>(read_u16_be(bytes.data() + 1))) / VALUE_TENTHS_PER_UNIT;

    case ValueCodec::THOUSANDTHS_U32_BE:
      return static_cast<float>(big_endian_u32(bytes)) / VALUE_THOUSANDTHS_PER_UNIT;

    case ValueCodec::ANY_NONZERO:
      // Every byte handed in belongs to the value: a slice of a register that several readings share.
      return std::ranges::any_of(bytes, [](uint8_t byte) { return byte != 0; }) ? 1.0F : 0.0F;

    case ValueCodec::NONE:
      break;
  }
  return std::nullopt;
}

size_t encode_value(const ValueForm &form, float value, std::span<uint8_t> out) {
  // NONE, a pair with a limit of its own and the read-only forms carry no value a write may send.
  if (form.codec == ValueCodec::NONE || form.codec == ValueCodec::FIRST_BYTE_OF_PAIR ||
      form.codec == ValueCodec::TENTHS_S16_BE || form.codec == ValueCodec::THOUSANDTHS_U32_BE ||
      form.codec == ValueCodec::ANY_NONZERO || form.codec == ValueCodec::MARKED_TENTHS_S16_BE ||
      out.size() < value_codec_length(form.codec))
    return 0;

  switch (form.codec) {
    case ValueCodec::TENTHS_U16_OR_ZERO:
      if (value == 0.0F) {
        put_little_endian_u16(0, out);
        return 2;
      }
      return encode_tenths(form, value, out);

    case ValueCodec::TENTHS_U16:
      return encode_tenths(form, value, out);

    case ValueCodec::HUNDREDTHS_S16: {
      if (!within_write_range(form, value, {-HUNDREDTHS_S16_MAX, HUNDREDTHS_S16_MAX}))
        return 0;
      const auto raw = static_cast<int16_t>(std::round(value * HUNDREDTHS_PER_UNIT));
      if (raw == VALUE_HUNDREDTHS_FAULT_RAW)
        return 0;  // that pattern means "sensor fault", not a value
      put_little_endian_u16(static_cast<uint16_t>(raw), out);
      return 2;
    }

    case ValueCodec::BINARY:
      if (value != 0.0F && value != 1.0F)
        return 0;
      out[0] = static_cast<uint8_t>(value);
      return 1;

    case ValueCodec::BOUNDED_BYTE:
      if (!value_is_bounded_integer(value, std::min(form.max_value, VALUE_BYTE_MAX)))
        return 0;
      out[0] = static_cast<uint8_t>(value);
      return 1;

    case ValueCodec::BOUNDED_U16:
      if (!value_is_bounded_integer(value, form.max_value))
        return 0;
      put_little_endian_u16(static_cast<uint16_t>(value), out);
      return 2;

    case ValueCodec::SCALED_U16:
      if (form.scale.size() < 2 || !std::isfinite(value) || value < static_cast<float>(form.scale.front().natural) ||
          value > static_cast<float>(form.scale.back().natural))
        return 0;
      put_little_endian_u16(scaled_raw_from_natural(form.scale, value), out);
      return 2;

    case ValueCodec::U16_BE:
      if (!value_is_bounded_integer(value, UINT16_MAX))
        return 0;
      out[0] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> BITS_PER_BYTE);
      out[1] = static_cast<uint8_t>(static_cast<uint16_t>(value) & LOW_BYTE_MASK);
      return 2;

    case ValueCodec::MARKED_U16_BE:
      if (!value_is_bounded_integer(value, UINT16_MAX))
        return 0;
      out[0] = VALUE_MARKER_PRESENT;
      out[1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> BITS_PER_BYTE);
      out[2] = static_cast<uint8_t>(static_cast<uint16_t>(value) & LOW_BYTE_MASK);
      return 3;

    case ValueCodec::NONE:
    case ValueCodec::FIRST_BYTE_OF_PAIR:
    case ValueCodec::TENTHS_S16_BE:
    case ValueCodec::THOUSANDTHS_U32_BE:
    case ValueCodec::ANY_NONZERO:
    case ValueCodec::MARKED_TENTHS_S16_BE:
      break;
  }
  return 0;
}

}  // namespace home_io_control
}  // namespace esphome
