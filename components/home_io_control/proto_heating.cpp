/// @file proto_heating.cpp
/// @brief Pure codec for the private register protocol of 2W heaters (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_protocol

#include "proto_heating.h"

#include "proto_constants.h"
#include "proto_private_registers.h"
#include "proto_sizes.h"
#include "proto_value_codecs.h"

#include <optional>
#include <span>

namespace esphome {
namespace home_io_control {

namespace {

/// Mask for the low byte of a 16-bit little-endian register value.
constexpr uint16_t LOW_BYTE_MASK = 0xFF;

/// Payload length of a read, and of a write that carries no value ({prefix, direction, reg-high, reg-low}).
constexpr size_t PAYLOAD_LEN_HEADER = PRIVATE_REGISTER_HEADER_SIZE;
/// Payload length for a write that carries one value byte (mode / presence / window detection).
constexpr size_t PAYLOAD_LEN_ONE_VALUE = PRIVATE_REGISTER_HEADER_SIZE + 1;
/// Payload length for a write of a 16-bit value (the setpoint).
constexpr size_t PAYLOAD_LEN_TWO_VALUE = PRIVATE_REGISTER_PAYLOAD_MAX_SIZE;

/// Highest regulator-state value the register defines (HeatingRegulatorState::HEATING).
constexpr uint8_t REGULATOR_STATE_MAX = static_cast<uint8_t>(HeatingRegulatorState::HEATING);
/// Highest towel-dryer mode value the register defines (TowelDryerMode::MANUAL).
constexpr uint8_t TOWEL_MODE_MAX = static_cast<uint8_t>(TowelDryerMode::MANUAL);
/// Highest mode value a reply carries (HeatingMode::OFF); the "external setpoint" mode below it is readable.
constexpr uint8_t HEATING_MODE_MAX = static_cast<uint8_t>(HeatingMode::OFF);
/// Highest temporary-state value (HeatingTemporaryState::DRYING).
constexpr uint8_t TEMPORARY_STATE_MAX = static_cast<uint8_t>(HeatingTemporaryState::DRYING);
/// Highest working rate: a percentage.
constexpr uint8_t WORKING_RATE_MAX = 100;

/// @brief Who may do what with a register.
enum class HeatingAccess : uint8_t {
  READ_ONLY,   ///< Reads only; a write is refused by the codec.
  READ_WRITE,  ///< Reads and writes.
  WRITE_ONLY,  ///< Writes only; the heater has no readable value for it.
};

/// @brief One register of the heater, the single description both directions are built from.
///
/// A request is {PRIVATE_REGISTER_PREFIX, direction, HEATING_REGISTER_HIGH_BYTE, register_low,
/// [value...]}; `direction` is PRIVATE_REGISTER_DIRECTION_GET or PRIVATE_REGISTER_DIRECTION_SET.
struct HeatingFunctionDescriptor {
  const char *name;       ///< Action-visible name.
  HeatingFunction fn;     ///< Function this row describes.
  uint8_t register_low;   ///< Payload byte 3: low byte of the 0x01xx register number.
  HeatingAccess access;   ///< Whether the register may be read, written or both.
  ValueCodec codec;       ///< How the value maps onto the trailing bytes.
  uint16_t max_value{0};  ///< BOUNDED_BYTE, BOUNDED_U16: highest value a reply may carry and a write may send.
};

constexpr HeatingFunctionDescriptor HEATING_FUNCTIONS[] = {
    {"power_on", HeatingFunction::POWER_ON, 0x2C, HeatingAccess::READ_ONLY, ValueCodec::NONE},
    {"set_temperature", HeatingFunction::SET_TEMPERATURE, 0x03, HeatingAccess::READ_WRITE, ValueCodec::TENTHS_U16},
    {"set_mode", HeatingFunction::SET_MODE, 0x00, HeatingAccess::READ_WRITE, ValueCodec::BOUNDED_BYTE,
     HEATING_MODE_MAX},
    // 0x0110 is written by a controller to say someone is there; a Sauter Hekla reports detected presence in it.
    {"set_presence", HeatingFunction::SET_PRESENCE, 0x10, HeatingAccess::READ_WRITE, ValueCodec::BINARY},
    {"set_open_window_detection", HeatingFunction::SET_OPEN_WINDOW_DETECTION, 0x06, HeatingAccess::READ_WRITE,
     ValueCodec::BINARY},
    {"midnight_sync", HeatingFunction::MIDNIGHT_SYNC, 0x30, HeatingAccess::READ_ONLY, ValueCodec::NONE},
    {"room_temperature", HeatingFunction::ROOM_TEMPERATURE, 0x15, HeatingAccess::READ_ONLY, ValueCodec::HUNDREDTHS_S16},
    {"regulator_state", HeatingFunction::REGULATOR_STATE, 0x17, HeatingAccess::READ_ONLY, ValueCodec::BOUNDED_BYTE,
     REGULATOR_STATE_MAX},
    {"max_setpoint", HeatingFunction::MAX_SETPOINT, 0x02, HeatingAccess::READ_ONLY, ValueCodec::TENTHS_U16},
    {"window_open", HeatingFunction::WINDOW_OPEN, 0x0E, HeatingAccess::READ_ONLY, ValueCodec::BINARY},
    // Towel-dryer registers: read on two Atlantic Adelis units (corpus atlantic_adelis_probe_*); writing
    // 0x0136 (mode) and 0x013E (boost, drying) is confirmed against the display (issue #202).
    {"towel_mode", HeatingFunction::TOWEL_MODE, 0x36, HeatingAccess::READ_WRITE, ValueCodec::BOUNDED_BYTE,
     TOWEL_MODE_MAX},
    {"temporary_state", HeatingFunction::TEMPORARY_STATE, 0x3E, HeatingAccess::READ_WRITE, ValueCodec::BOUNDED_BYTE,
     TEMPORARY_STATE_MAX},
    {"working_rate", HeatingFunction::WORKING_RATE, 0x26, HeatingAccess::READ_ONLY, ValueCodec::BOUNDED_BYTE,
     WORKING_RATE_MAX},
    // Read on a towel dryer in a programme period (issue #202): the temperature in force in 0x0137 and the
    // eco setback in 0x0104.
    {"temperature_in_force", HeatingFunction::TEMPERATURE_IN_FORCE, 0x37, HeatingAccess::READ_ONLY,
     ValueCodec::TENTHS_U16},
    {"eco_setback", HeatingFunction::ECO_SETBACK, 0x04, HeatingAccess::READ_ONLY, ValueCodec::TENTHS_U16},
    // Read during a panel override on a towel dryer (issue #202): 16.5 degrees, 0 otherwise.
    {"set_derogation", HeatingFunction::DEROGATED_SETPOINT, 0x1D, HeatingAccess::READ_WRITE,
     ValueCodec::TENTHS_U16_OR_ZERO},
    // Not read on any radiator yet; the same temperature as 0x0137 on a towel dryer, which read it in both.
    {"heater_temperature_in_force", HeatingFunction::HEATER_TEMPERATURE_IN_FORCE, 0x2A, HeatingAccess::READ_ONLY,
     ValueCodec::TENTHS_U16},
    // Night light registers: read on a Sauter Hekla with the light on and off (issue #222).
    {"night_light", HeatingFunction::NIGHT_LIGHT, 0x44, HeatingAccess::READ_WRITE, ValueCodec::BINARY},
    {"night_light_brightness", HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, 0x42, HeatingAccess::READ_WRITE,
     ValueCodec::BOUNDED_U16, HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX},
    // Read on a mixed towel dryer (issue #243): 1 while its central-heating mode is on, 0 in electric mode.
    {"central_heating_mode", HeatingFunction::CENTRAL_HEATING_MODE, 0x53, HeatingAccess::READ_ONLY, ValueCodec::BINARY},
    // Read and written on a Sauter Hekla (issue #222): 1 with its occupancy detection on, 0 with it off.
    {"occupancy_detection", HeatingFunction::OCCUPANCY_DETECTION, 0x01, HeatingAccess::READ_WRITE, ValueCodec::BINARY},
    // Read on two Sauter Tahual radiators, equal to their displays (PR #238), and on an EMOTION 3 H and two Adelis.
    {"energy_total", HeatingFunction::ENERGY_TOTAL, 0x16, HeatingAccess::READ_ONLY, ValueCodec::U16_BE},
    // Read on Atlantic Adelis towel dryers during a boost and a drying run, and with the durations changed on the
    // panel (issue #202): the minutes left of each run, and the duration set for each with the panel's maximum.
    {"boost_remaining", HeatingFunction::BOOST_REMAINING, 0x3A, HeatingAccess::READ_ONLY, ValueCodec::BOUNDED_BYTE,
     VALUE_BYTE_MAX},
    {"boost_duration", HeatingFunction::BOOST_DURATION, 0x3B, HeatingAccess::READ_ONLY, ValueCodec::FIRST_BYTE_OF_PAIR},
    {"drying_remaining", HeatingFunction::DRYING_REMAINING, 0x3C, HeatingAccess::READ_ONLY, ValueCodec::BOUNDED_BYTE,
     VALUE_BYTE_MAX},
    {"drying_duration", HeatingFunction::DRYING_DURATION, 0x3D, HeatingAccess::READ_ONLY,
     ValueCodec::FIRST_BYTE_OF_PAIR},
};

/// @brief Look up the descriptor for a function.
/// @param fn Function to resolve.
/// @return Pointer into the constexpr table, or nullptr for an out-of-range value.
const HeatingFunctionDescriptor *descriptor_for(HeatingFunction fn) {
  for (const auto &d : HEATING_FUNCTIONS) {
    if (d.fn == fn)
      return &d;
  }
  return nullptr;
}

/// @brief Write the 4-byte request header for a row.
void write_header(const HeatingFunctionDescriptor &d, uint8_t direction, uint8_t *out) {
  write_private_register_header(direction, HEATING_REGISTER_HIGH_BYTE, d.register_low, out);
}

/// @brief Is `value` an integer from 0 to `max_value`? NaN fails every comparison, so it is refused too.
bool is_bounded_integer(float value, uint16_t max_value) {
  return std::isfinite(value) && value >= 0.0F && value <= static_cast<float>(max_value) && value == std::floor(value);
}

/// @brief Write a set request for a 16-bit register: the header, then the value low byte first, the
/// order the register reads back in.
/// @return PAYLOAD_LEN_TWO_VALUE.
size_t write_u16_set(const HeatingFunctionDescriptor &d, uint16_t raw, uint8_t *out) {
  write_header(d, PRIVATE_REGISTER_DIRECTION_SET, out);
  out[4] = static_cast<uint8_t>(raw & LOW_BYTE_MASK);
  out[PAYLOAD_LEN_TWO_VALUE - 1] = static_cast<uint8_t>(raw >> BITS_PER_BYTE);
  return PAYLOAD_LEN_TWO_VALUE;
}

}  // namespace

const char *heating_function_name(HeatingFunction fn) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  return d != nullptr ? d->name : "unknown";
}

bool heating_function_is_writable(HeatingFunction fn) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  return d != nullptr && d->access != HeatingAccess::READ_ONLY;
}

bool heating_function_is_readable(HeatingFunction fn) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  return d != nullptr && d->access != HeatingAccess::WRITE_ONLY;
}

bool heating_function_register(HeatingFunction fn, uint8_t &register_low) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr)
    return false;
  register_low = d->register_low;
  return true;
}

size_t encode_heating_read(HeatingFunction fn, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr || d->access == HeatingAccess::WRITE_ONLY)
    return 0;
  write_header(*d, PRIVATE_REGISTER_DIRECTION_GET, out);
  return PAYLOAD_LEN_HEADER;
}

/// @brief Per-register rule on top of the codec's range check: refuses values this component never writes
/// although the register's range holds them. Today only the mode register has one (heating_mode_is_writable()).
/// @return false only for a value the rule refuses; a value the codec must judge passes.
static bool value_is_writable(HeatingFunction fn, float value) {
  if (fn != HeatingFunction::SET_MODE)
    return true;
  // A non-integral, out-of-range or non-finite value is the codec's to refuse; only a whole mode number is judged.
  return !is_bounded_integer(value, UINT8_MAX) ||
         heating_mode_is_writable(static_cast<HeatingMode>(static_cast<uint8_t>(value)));
}

size_t encode_heating_write(HeatingFunction fn, float value, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  // A read-only register is never written, whatever the caller asks: the heater's window sensor and
  // its measurements are not settings.
  if (d == nullptr || d->access == HeatingAccess::READ_ONLY)
    return 0;

  if (!value_is_writable(fn, value))
    return 0;

  switch (d->codec) {
    case ValueCodec::TENTHS_U16_OR_ZERO:
      if (value == 0.0F)
        return write_u16_set(*d, 0, out);
      [[fallthrough]];
    case ValueCodec::TENTHS_U16: {
      // Reject NaN / infinity and anything outside the representable range rather than emitting a
      // wrapped byte.
      if (!std::isfinite(value) || value < HEATING_TEMP_MIN_C || value > HEATING_TEMP_MAX_C)
        return 0;
      // Multiply in float (not double): the float32 product of e.g. 20.55f * 10.0f is exactly
      // 205.5f, and std::round() takes that half away from zero to 206 (0x00CE). Truncating or
      // rounding toward even would both be wrong here.
      // NB: this exactness relies on FLT_EVAL_METHOD == 0 (operands evaluated at their own single
      // precision, no hidden widening to double). That holds on x86-64 SSE and on Xtensa; the
      // proto_heating test pins 20.55 C, which is one ULP from flipping to 0xCD under any
      // excess-precision evaluation.
      return write_u16_set(*d, static_cast<uint16_t>(std::round(value * VALUE_TENTHS_PER_UNIT)), out);
    }

    case ValueCodec::BINARY:
      if (value != 0.0F && value != 1.0F)
        return 0;
      write_header(*d, PRIVATE_REGISTER_DIRECTION_SET, out);
      out[4] = static_cast<uint8_t>(value);
      return PAYLOAD_LEN_ONE_VALUE;

    case ValueCodec::BOUNDED_BYTE:
      // An integral value inside the register's range; anything else is refused, never truncated.
      if (!is_bounded_integer(value, d->max_value))
        return 0;
      write_header(*d, PRIVATE_REGISTER_DIRECTION_SET, out);
      out[4] = static_cast<uint8_t>(value);
      return PAYLOAD_LEN_ONE_VALUE;

    case ValueCodec::BOUNDED_U16:
      // The same rule, in the two-byte form.
      if (!is_bounded_integer(value, d->max_value))
        return 0;
      return write_u16_set(*d, static_cast<uint16_t>(value), out);

    case ValueCodec::NONE:
    case ValueCodec::HUNDREDTHS_S16:
    case ValueCodec::U16_BE:
    case ValueCodec::MARKED_U16_BE:
    case ValueCodec::FIRST_BYTE_OF_PAIR:
      return 0;  // no writable register has this encoding
  }
  return 0;
}

size_t encode_heating_payload(HeatingFunction fn, float value, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr)
    return 0;
  return d->access == HeatingAccess::READ_ONLY ? encode_heating_read(fn, out) : encode_heating_write(fn, value, out);
}

bool decode_heating_reply(const IoFrame &reply, HeatingFunction fn, float &value) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr || d->codec == ValueCodec::NONE ||
      !private_reply_echoes_register(reply, HEATING_REGISTER_HIGH_BYTE, d->register_low))
    return false;
  if (reply.data_len < PRIVATE_REGISTER_VALUE_OFFSET)
    return false;

  const std::optional<float> decoded =
      decode_value(d->codec,
                   std::span<const uint8_t>(reply.data + PRIVATE_REGISTER_VALUE_OFFSET,
                                            reply.data_len - PRIVATE_REGISTER_VALUE_OFFSET),
                   d->max_value);
  if (!decoded)
    return false;
  value = *decoded;
  return true;
}

bool heating_reply_carries_value(const IoFrame &reply, HeatingFunction fn) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr || d->codec == ValueCodec::NONE ||
      !private_reply_echoes_register(reply, HEATING_REGISTER_HIGH_BYTE, d->register_low) ||
      reply.data_len < PRIVATE_REGISTER_VALUE_OFFSET)
    return false;
  return reply.data_len - PRIVATE_REGISTER_VALUE_OFFSET >= value_codec_length(d->codec);
}

}  // namespace home_io_control
}  // namespace esphome
