/// @file proto_heating.cpp
/// @brief Pure codec for the private register protocol of 2W heaters (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_protocol

#include "proto_heating.h"

#include "proto_constants.h"
#include "proto_sizes.h"

namespace esphome {
namespace home_io_control {

namespace {

/// Mask for the low byte of a 16-bit little-endian register value.
constexpr uint16_t LOW_BYTE_MASK = 0xFF;

/// Payload length of a read, and of a write that carries no value ({prefix, direction, reg-high, reg-low}).
constexpr size_t PAYLOAD_LEN_HEADER = 4;
/// Payload length for a write that carries one value byte (mode / presence / window detection).
constexpr size_t PAYLOAD_LEN_ONE_VALUE = 5;
/// Payload length for a write of a 16-bit value (the setpoint).
constexpr size_t PAYLOAD_LEN_TWO_VALUE = HEATING_PAYLOAD_MAX_SIZE;

/// Wire scale of the setpoint registers: round(degrees * this), little-endian 16-bit.
constexpr float DEGREES_TO_TENTHS = 10.0F;
/// Scale of the room-temperature register: signed hundredths of a degree.
constexpr float HUNDREDTHS_PER_DEGREE = 100.0F;

/// Highest regulator-state value the register defines (HeatingRegulatorState::HEATING).
constexpr uint8_t REGULATOR_STATE_MAX = static_cast<uint8_t>(HeatingRegulatorState::HEATING);

/// @brief How a register's value maps onto the bytes after the 4-byte header.
enum class HeatingValueKind : uint8_t {
  NONE,            ///< No typed value: logged raw, never decoded.
  TENTHS_U16,      ///< Unsigned 16-bit little-endian tenths of a degree.
  HUNDREDTHS_S16,  ///< Signed 16-bit little-endian hundredths of a degree; -1 is a sensor fault.
  BINARY,          ///< One byte, 0 or 1.
  MODE,            ///< One HeatingMode byte.
  ENUM_BYTE,       ///< One byte of a small enumeration (regulator state).
};

/// @brief Who may do what with a register.
enum class HeatingAccess : uint8_t {
  READ_ONLY,   ///< Reads only; a write is refused by the codec.
  READ_WRITE,  ///< Reads and writes.
  WRITE_ONLY,  ///< Writes only; the heater has no readable value for it.
};

/// @brief One register of the heater, the single description both directions are built from.
///
/// A request is {HEATING_PAYLOAD_PREFIX, direction, HEATING_REGISTER_HIGH_BYTE, register_low,
/// [value...]}; `direction` is HEATING_DIRECTION_GET or HEATING_DIRECTION_SET.
struct HeatingFunctionDescriptor {
  const char *name;       ///< Action-visible name.
  HeatingFunction fn;     ///< Function this row describes.
  uint8_t register_low;   ///< Payload byte 3: low byte of the 0x01xx register number.
  HeatingAccess access;   ///< Whether the register may be read, written or both.
  HeatingValueKind kind;  ///< How the value maps onto the trailing bytes.
};

constexpr HeatingFunctionDescriptor HEATING_FUNCTIONS[] = {
    {"power_on", HeatingFunction::POWER_ON, 0x2C, HeatingAccess::READ_ONLY, HeatingValueKind::NONE},
    {"set_temperature", HeatingFunction::SET_TEMPERATURE, 0x03, HeatingAccess::READ_WRITE,
     HeatingValueKind::TENTHS_U16},
    {"set_mode", HeatingFunction::SET_MODE, 0x00, HeatingAccess::READ_WRITE, HeatingValueKind::MODE},
    {"set_presence", HeatingFunction::SET_PRESENCE, 0x10, HeatingAccess::WRITE_ONLY, HeatingValueKind::BINARY},
    {"set_open_window_detection", HeatingFunction::SET_OPEN_WINDOW_DETECTION, 0x06, HeatingAccess::READ_WRITE,
     HeatingValueKind::BINARY},
    {"midnight_sync", HeatingFunction::MIDNIGHT_SYNC, 0x30, HeatingAccess::READ_ONLY, HeatingValueKind::NONE},
    {"room_temperature", HeatingFunction::ROOM_TEMPERATURE, 0x15, HeatingAccess::READ_ONLY,
     HeatingValueKind::HUNDREDTHS_S16},
    {"regulator_state", HeatingFunction::REGULATOR_STATE, 0x17, HeatingAccess::READ_ONLY, HeatingValueKind::ENUM_BYTE},
    {"max_setpoint", HeatingFunction::MAX_SETPOINT, 0x02, HeatingAccess::READ_ONLY, HeatingValueKind::TENTHS_U16},
    {"window_open", HeatingFunction::WINDOW_OPEN, 0x0E, HeatingAccess::READ_ONLY, HeatingValueKind::BINARY},
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
  out[0] = HEATING_PAYLOAD_PREFIX;
  out[1] = direction;
  out[2] = HEATING_REGISTER_HIGH_BYTE;
  out[3] = d.register_low;
}

/// @brief Number of value bytes behind the header for a kind.
size_t value_length(HeatingValueKind kind) {
  switch (kind) {
    case HeatingValueKind::NONE:
      return 0;
    case HeatingValueKind::TENTHS_U16:
    case HeatingValueKind::HUNDREDTHS_S16:
      return 2;
    case HeatingValueKind::BINARY:
    case HeatingValueKind::MODE:
    case HeatingValueKind::ENUM_BYTE:
      return 1;
  }
  return 0;
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

size_t encode_heating_read(HeatingFunction fn, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr || d->access == HeatingAccess::WRITE_ONLY)
    return 0;
  write_header(*d, HEATING_DIRECTION_GET, out);
  return PAYLOAD_LEN_HEADER;
}

size_t encode_heating_write(HeatingFunction fn, float value, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  // A read-only register is never written, whatever the caller asks: the heater's window sensor and
  // its measurements are not settings.
  if (d == nullptr || d->access == HeatingAccess::READ_ONLY)
    return 0;

  switch (d->kind) {
    case HeatingValueKind::TENTHS_U16: {
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
      const auto tenths = static_cast<uint16_t>(std::round(value * DEGREES_TO_TENTHS));
      write_header(*d, HEATING_DIRECTION_SET, out);
      out[4] = static_cast<uint8_t>(tenths & LOW_BYTE_MASK);
      out[PAYLOAD_LEN_TWO_VALUE - 1] = static_cast<uint8_t>(tenths >> BITS_PER_BYTE);
      return PAYLOAD_LEN_TWO_VALUE;
    }

    case HeatingValueKind::MODE:
      // Exact float match against the four modes this component writes; a non-integral or unknown
      // value (the "external" mode included) is rejected, not coerced.
      if (value != static_cast<float>(HeatingMode::AUTO) && value != static_cast<float>(HeatingMode::MANUAL) &&
          value != static_cast<float>(HeatingMode::PROG) && value != static_cast<float>(HeatingMode::OFF))
        return 0;
      write_header(*d, HEATING_DIRECTION_SET, out);
      out[4] = static_cast<uint8_t>(value);
      return PAYLOAD_LEN_ONE_VALUE;

    case HeatingValueKind::BINARY:
      if (value != 0.0F && value != 1.0F)
        return 0;
      write_header(*d, HEATING_DIRECTION_SET, out);
      out[4] = static_cast<uint8_t>(value);
      return PAYLOAD_LEN_ONE_VALUE;

    case HeatingValueKind::NONE:
    case HeatingValueKind::HUNDREDTHS_S16:
    case HeatingValueKind::ENUM_BYTE:
      return 0;  // no writable register has this encoding
  }
  return 0;
}

size_t encode_heating_payload(HeatingFunction fn, float value, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr)
    return 0;
  return d->access == HeatingAccess::READ_ONLY ? encode_heating_read(fn, out) : encode_heating_write(fn, value, out);
}

bool decode_heating_reply(const IoFrame &reply, HeatingFunction fn, float &value) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr || d->kind == HeatingValueKind::NONE || reply.cmd != CMD_WRITE_PRIVATE_ACK)
    return false;

  const size_t value_len = value_length(d->kind);
  if (reply.data_len < PAYLOAD_LEN_HEADER + value_len)
    return false;
  const uint8_t *data = reply.data;
  if (data[0] != HEATING_PAYLOAD_PREFIX || data[2] != HEATING_REGISTER_HIGH_BYTE || data[3] != d->register_low)
    return false;
  if (data[1] != HEATING_DIRECTION_GET && data[1] != HEATING_DIRECTION_SET)
    return false;

  const uint8_t *v = data + PAYLOAD_LEN_HEADER;
  const auto raw16 = static_cast<uint16_t>(v[0] | (v[1] << BITS_PER_BYTE));
  switch (d->kind) {
    case HeatingValueKind::TENTHS_U16:
      value = static_cast<float>(raw16) / DEGREES_TO_TENTHS;
      return true;

    case HeatingValueKind::HUNDREDTHS_S16: {
      const auto signed_raw = static_cast<int16_t>(raw16);
      value = signed_raw == HEATING_SENSOR_FAULT_RAW ? NAN : static_cast<float>(signed_raw) / HUNDREDTHS_PER_DEGREE;
      return true;
    }

    case HeatingValueKind::BINARY:
      if (v[0] > 1)
        return false;
      value = static_cast<float>(v[0]);
      return true;

    case HeatingValueKind::MODE:
      if (v[0] > static_cast<uint8_t>(HeatingMode::OFF))
        return false;
      value = static_cast<float>(v[0]);
      return true;

    case HeatingValueKind::ENUM_BYTE:
      if (v[0] > REGULATOR_STATE_MAX)
        return false;
      value = static_cast<float>(v[0]);
      return true;

    case HeatingValueKind::NONE:
      return false;
  }
  return false;
}

bool HeatingReadings::store(HeatingFunction fn, float value) {
  switch (fn) {
    case HeatingFunction::ROOM_TEMPERATURE:
      this->room_temperature_c = value;
      return true;
    case HeatingFunction::SET_TEMPERATURE:
      this->setpoint_c = value;
      return true;
    case HeatingFunction::MAX_SETPOINT:
      this->max_setpoint_c = value;
      return true;
    case HeatingFunction::SET_MODE:
      this->mode = static_cast<int16_t>(value);
      return true;
    case HeatingFunction::REGULATOR_STATE:
      this->regulator = static_cast<int16_t>(value);
      return true;
    case HeatingFunction::WINDOW_OPEN:
      this->window_open = static_cast<int16_t>(value);
      return true;
    case HeatingFunction::POWER_ON:
    case HeatingFunction::SET_PRESENCE:
    case HeatingFunction::SET_OPEN_WINDOW_DETECTION:
    case HeatingFunction::MIDNIGHT_SYNC:
      return false;
  }
  return false;
}

}  // namespace home_io_control
}  // namespace esphome
