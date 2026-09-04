/// @file proto_heating.cpp
/// @brief Pure codec for IO-Homecontrol 2W heating/climate functions (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_protocol
///
/// Byte layout cited throughout to reference/iohomecontrol/src/iohcCozyDevice2W.cpp; the
/// direction/register bytes and the 16-bit little-endian tenths-of-a-degree setpoint encoding are
/// from reference/iown-homecontrol/docs/devices/misc/AtlanticThermor/README.md.

#include "proto_heating.h"

#include "proto_sizes.h"

#include <cmath>

namespace esphome {
namespace home_io_control {

namespace {

/// Mask for the low byte of the 16-bit little-endian setpoint field.
constexpr uint16_t SETPOINT_LOW_BYTE_MASK = 0xFF;

/// Payload length for a function that carries no value ({prefix, direction, reg-high, reg-low}).
constexpr size_t PAYLOAD_LEN_NO_VALUE = 4;
/// Payload length for a function that carries one value byte (mode / presence / window).
constexpr size_t PAYLOAD_LEN_ONE_VALUE = 5;
/// Payload length for SET_TEMPERATURE (a 16-bit little-endian tenths-of-a-degree setpoint).
constexpr size_t PAYLOAD_LEN_TEMPERATURE = HEATING_PAYLOAD_MAX_SIZE;
/// Wire scale: the setpoint is round(degrees * this), little-endian 16-bit
/// (AtlanticThermor/README.md's 0x0103 / 0x0130 rows; iohcCozyDevice2W.cpp:127-128 does the
/// multiply but only stores the low byte).
constexpr float DEGREES_TO_TENTHS = 10.0F;

/// @brief How a function's value maps onto the payload's trailing bytes.
enum class HeatingValueKind : uint8_t {
  NONE,         ///< No trailing value — 4-byte payload.
  TEMPERATURE,  ///< A 16-bit little-endian tenths-of-a-degree setpoint — 6-byte payload.
  MODE,         ///< One HeatingMode byte — 5-byte payload.
  BINARY,       ///< One 0/1 byte — 5-byte payload.
};

/// @brief Per-function wire descriptor.
///
/// A payload is {HEATING_PAYLOAD_PREFIX, direction, HEATING_REGISTER_HIGH_BYTE, register_low,
/// [value...]}. `direction` is 0x60 (get/read) or 0x61 (set/write) and `register_low` is the low
/// byte of the 16-bit register number, per
/// reference/iown-homecontrol/docs/devices/misc/AtlanticThermor/README.md ("Set 0c61 01xx" /
/// "Get 0c60 01xx").
struct HeatingFunctionDescriptor {
  HeatingFunction fn;     ///< Function this row describes.
  uint8_t direction;      ///< Payload byte 1: 0x60 = get/read, 0x61 = set/write.
  uint8_t register_low;   ///< Payload byte 3: low byte of the 0x01xx register number.
  HeatingValueKind kind;  ///< How this function's value maps onto the trailing payload byte(s).
};

/// Direction / register-low / value-kind per function. Payload forms (iohcCozyDevice2W.cpp;
/// registers per AtlanticThermor/README.md):
///   POWER_ON        :105  {0x0C, 0x60, 0x01, 0x2C}            get reg 0x012C (paired-device list)
///   SET_TEMPERATURE :125  {0x0C, 0x61, 0x01, 0x03, lo, hi}    set reg 0x0103, LE16 tenths
///   SET_MODE        :155  {0x0C, 0x61, 0x01, 0x00, mode}      set reg 0x0100
///   SET_PRESENCE    :195  {0x0C, 0x61, 0x01, 0x10, val}       set reg 0x0110
///   SET_WINDOW      :218  {0x0C, 0x61, 0x01, 0x0E, val}       set reg 0x010E
///   MIDNIGHT_SYNC   :248  {0x0c, 0x60, 0x01, 0x30}            get reg 0x0130 (setpoint block)
constexpr HeatingFunctionDescriptor HEATING_FUNCTIONS[] = {
    {HeatingFunction::POWER_ON, 0x60, 0x2C, HeatingValueKind::NONE},
    {HeatingFunction::SET_TEMPERATURE, 0x61, 0x03, HeatingValueKind::TEMPERATURE},
    {HeatingFunction::SET_MODE, 0x61, 0x00, HeatingValueKind::MODE},
    {HeatingFunction::SET_PRESENCE, 0x61, 0x10, HeatingValueKind::BINARY},
    {HeatingFunction::SET_WINDOW, 0x61, 0x0E, HeatingValueKind::BINARY},
    {HeatingFunction::MIDNIGHT_SYNC, 0x60, 0x30, HeatingValueKind::NONE},
};

/// @brief Look up the wire descriptor for a function.
/// @param fn Function to resolve.
/// @return Pointer into the constexpr HEATING_FUNCTIONS table, or nullptr for an out-of-range value.
const HeatingFunctionDescriptor *descriptor_for(HeatingFunction fn) {
  for (const auto &d : HEATING_FUNCTIONS) {
    if (d.fn == fn)
      return &d;
  }
  return nullptr;
}

}  // namespace

const char *heating_function_name(HeatingFunction fn) {
  switch (fn) {
    case HeatingFunction::POWER_ON:
      return "power_on";
    case HeatingFunction::SET_TEMPERATURE:
      return "set_temperature";
    case HeatingFunction::SET_MODE:
      return "set_mode";
    case HeatingFunction::SET_PRESENCE:
      return "set_presence";
    case HeatingFunction::SET_WINDOW:
      return "set_window";
    case HeatingFunction::MIDNIGHT_SYNC:
      return "midnight_sync";
  }
  return "unknown";
}

size_t encode_heating_payload(HeatingFunction fn, float value, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  if (d == nullptr)
    return 0;

  // Common prefix — {PREFIX, direction, REGISTER_HIGH_BYTE, register_low} — shared by every
  // function (iohcCozyDevice2W.cpp:105 / :125 / :155 / :195 / :218 / :248; direction and register
  // per AtlanticThermor/README.md).
  out[0] = HEATING_PAYLOAD_PREFIX;
  out[1] = d->direction;
  out[2] = HEATING_REGISTER_HIGH_BYTE;
  out[3] = d->register_low;

  switch (d->kind) {
    case HeatingValueKind::NONE:
      return PAYLOAD_LEN_NO_VALUE;

    case HeatingValueKind::TEMPERATURE: {
      // Reject NaN / infinity and anything outside the representable range rather than emitting a
      // wrapped byte (the reference's latent truncation bug, iohcCozyDevice2W.cpp:127-128).
      if (!std::isfinite(value) || value < HEATING_TEMP_MIN_C || value > HEATING_TEMP_MAX_C)
        return 0;
      // Multiply in float (not double): the float32 product of e.g. 20.55f * 10.0f is exactly
      // 205.5f, and std::round() takes that half away from zero to 206 (0x00CE). Truncating (as
      // the reference does) or rounding toward even would both be wrong here.
      // NB: this exactness relies on FLT_EVAL_METHOD == 0 (operands evaluated at their own
      // single precision, no hidden widening to double). That holds on x86-64 SSE and on Xtensa;
      // the proto_heating test pins 20.55 C, which is one ULP from flipping to 0xCD under any
      // excess-precision evaluation.
      // The setpoint field is a 16-bit little-endian value in tenths of a degree
      // (AtlanticThermor/README.md's 0x0103 / 0x0130 rows show `cd 00`, `c3 00`, `18 01` = 28.0).
      // For every value <= 25.5 C the high byte is 0x00, so the emitted bytes are identical to the
      // single-byte form iohcCozyDevice2W.cpp:125-128 writes.
      const auto tenths = static_cast<uint16_t>(std::round(value * DEGREES_TO_TENTHS));
      out[4] = static_cast<uint8_t>(tenths & SETPOINT_LOW_BYTE_MASK);
      out[PAYLOAD_LEN_TEMPERATURE - 1] = static_cast<uint8_t>(tenths >> BITS_PER_BYTE);
      return PAYLOAD_LEN_TEMPERATURE;
    }

    case HeatingValueKind::MODE:
      // Exact float match against the four exposed modes (iohcCozyDevice2W.cpp:158-162); a
      // non-integral or unknown value is rejected, not coerced.
      if (value != static_cast<float>(HeatingMode::AUTO) && value != static_cast<float>(HeatingMode::MANUAL) &&
          value != static_cast<float>(HeatingMode::PROG) && value != static_cast<float>(HeatingMode::OFF))
        return 0;
      out[4] = static_cast<uint8_t>(value);
      return PAYLOAD_LEN_ONE_VALUE;

    case HeatingValueKind::BINARY:
      // 0 or 1 only (iohcCozyDevice2W.cpp:198-199 for presence, :221-222 for window).
      if (value != 0.0F && value != 1.0F)
        return 0;
      out[4] = static_cast<uint8_t>(value);
      return PAYLOAD_LEN_ONE_VALUE;
  }
  return 0;
}

}  // namespace home_io_control
}  // namespace esphome
