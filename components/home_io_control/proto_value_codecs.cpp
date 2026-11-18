/// @file proto_value_codecs.cpp
/// @brief The value encodings of the bytes a device reports, and the one pure decoder for them.
/// @ingroup hioc_protocol

#include "proto_value_codecs.h"

#include "proto_sizes.h"

#include <cmath>

namespace esphome {
namespace home_io_control {

namespace {

/// Scale of HUNDREDTHS_S16: signed hundredths of a unit.
constexpr float HUNDREDTHS_PER_UNIT = 100.0F;

/// @return The 16-bit little-endian word at the start of `bytes` (two bytes must be there).
uint16_t little_endian_u16(std::span<const uint8_t> bytes) {
  return static_cast<uint16_t>(bytes[0] | (bytes[1] << BITS_PER_BYTE));
}

}  // namespace

size_t value_codec_length(ValueCodec codec) {
  switch (codec) {
    case ValueCodec::NONE:
      return 0;
    case ValueCodec::TENTHS_U16:
    case ValueCodec::TENTHS_U16_OR_ZERO:
    case ValueCodec::HUNDREDTHS_S16:
    case ValueCodec::BOUNDED_U16:
    case ValueCodec::U16_BE:
    case ValueCodec::FIRST_BYTE_OF_PAIR:
      return 2;
    case ValueCodec::BINARY:
    case ValueCodec::BOUNDED_BYTE:
      return 1;
  }
  return 0;
}

std::optional<float> decode_value(ValueCodec codec, std::span<const uint8_t> bytes, uint16_t max_value) {
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

    case ValueCodec::U16_BE:
      // Most significant byte first, unlike every little-endian word above.
      return static_cast<float>((bytes[0] << BITS_PER_BYTE) | bytes[1]);

    case ValueCodec::FIRST_BYTE_OF_PAIR:
      // The second byte (the panel's maximum) is not read.
      return static_cast<float>(bytes[0]);

    case ValueCodec::NONE:
      break;
  }
  return std::nullopt;
}

}  // namespace home_io_control
}  // namespace esphome
