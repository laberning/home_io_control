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
#include <string>
#include <string_view>

namespace esphome {
namespace home_io_control {

namespace {

/// Payload length of a read ({prefix, direction, reg-high, reg-low}).
constexpr size_t PAYLOAD_LEN_HEADER = PRIVATE_REGISTER_HEADER_SIZE;
/// Room for the value bytes of a write after the header: one byte, or a 16-bit value.
constexpr size_t PAYLOAD_LEN_VALUE_MAX = PRIVATE_REGISTER_PAYLOAD_MAX_SIZE - PRIVATE_REGISTER_HEADER_SIZE;

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

/// @brief A register's refusal of values its range holds but this component never writes.
/// @return true for a value the register refuses; false for one the codec alone must judge.
using HeatingValueRefusal = bool (*)(float value);

/// @brief One register of the heater, the single description both directions are built from.
///
/// A request is {PRIVATE_REGISTER_PREFIX, direction, HEATING_REGISTER_HIGH_BYTE, register_low,
/// [value...]}; `direction` is PRIVATE_REGISTER_DIRECTION_GET or PRIVATE_REGISTER_DIRECTION_SET. The row is
/// also the register's write rule: its codec, `max_value`, `write_range` and `refuses` say which values a write
/// may carry, and every caller asks heating_write_accepts() instead of keeping a bound of its own.
struct HeatingFunctionDescriptor {
  const char *name;       ///< Action-visible name.
  HeatingFunction fn;     ///< Function this row describes.
  uint8_t register_low;   ///< Payload byte 3: low byte of the 0x01xx register number.
  HeatingAccess access;   ///< Whether the register may be read, written or both.
  ValueCodec codec;       ///< How the value maps onto the trailing bytes.
  uint16_t max_value{0};  ///< BOUNDED_BYTE, BOUNDED_U16: highest value a reply may carry and a write may send.
  // NOLINTBEGIN(readability-redundant-member-init): g++'s -Wmissing-field-initializers wants the member
  // initializer before a row may leave the field out.
  std::optional<ValueRange> write_range{};  ///< Tenths registers: the values a write may carry.
  HeatingValueRefusal refuses{nullptr};     ///< A value the register refuses although the codec holds it.
  /// The `heating_control` action offers this function under `name`. The label names the value in the action's
  /// messages ("temperature", "mode"); "" for a function that takes no value (a read). nullptr: not a function of
  /// the action.
  const char *action_value_label{nullptr};
  std::span<const ValueScaleKnot> scale{};  ///< SCALED_U16: the scale from the natural value to the register value.
  // NOLINTEND(readability-redundant-member-init)
};

/// @brief The mode register refuses the modes heating_mode_is_writable() rejects.
/// @details Only a whole mode number is judged here; a non-integral, out-of-range or non-finite value is the
///          codec's to refuse.
bool refuses_unwritable_mode(float value) {
  return value_is_bounded_integer(value, UINT8_MAX) &&
         !heating_mode_is_writable(static_cast<HeatingMode>(static_cast<uint8_t>(value)));
}

constexpr HeatingFunctionDescriptor HEATING_FUNCTIONS[] = {
    {.name = "power_on",
     .fn = HeatingFunction::POWER_ON,
     .register_low = 0x2C,
     .access = HeatingAccess::READ_ONLY,
     .codec = ValueCodec::NONE,
     .action_value_label = ""},
    {.name = "set_temperature",
     .fn = HeatingFunction::SET_TEMPERATURE,
     .register_low = 0x03,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::TENTHS_U16,
     .write_range = ValueRange{HEATING_TEMP_MIN_C, HEATING_TEMP_MAX_C},
     .action_value_label = "temperature"},
    {.name = "set_mode",
     .fn = HeatingFunction::SET_MODE,
     .register_low = 0x00,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::BOUNDED_BYTE,
     .max_value = HEATING_MODE_MAX,
     .refuses = refuses_unwritable_mode,
     .action_value_label = "mode"},
    // 0x0110 is written by a controller to say someone is there; a Sauter Hekla reports detected presence in it.
    {.name = "set_presence",
     .fn = HeatingFunction::SET_PRESENCE,
     .register_low = 0x10,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::BINARY,
     .action_value_label = "presence"},
    {.name = "set_open_window_detection",
     .fn = HeatingFunction::SET_OPEN_WINDOW_DETECTION,
     .register_low = 0x06,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::BINARY,
     .action_value_label = "open-window detection"},
    {.name = "midnight_sync",
     .fn = HeatingFunction::MIDNIGHT_SYNC,
     .register_low = 0x30,
     .access = HeatingAccess::READ_ONLY,
     .codec = ValueCodec::NONE,
     .action_value_label = ""},
    {"room_temperature", HeatingFunction::ROOM_TEMPERATURE, 0x15, HeatingAccess::READ_ONLY, ValueCodec::HUNDREDTHS_S16},
    {"regulator_state", HeatingFunction::REGULATOR_STATE, 0x17, HeatingAccess::READ_ONLY, ValueCodec::BOUNDED_BYTE,
     REGULATOR_STATE_MAX},
    {"max_setpoint", HeatingFunction::MAX_SETPOINT, 0x02, HeatingAccess::READ_ONLY, ValueCodec::TENTHS_U16},
    {.name = "window_open",
     .fn = HeatingFunction::WINDOW_OPEN,
     .register_low = 0x0E,
     .access = HeatingAccess::READ_ONLY,
     .codec = ValueCodec::BINARY,
     .action_value_label = ""},
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
    {.name = "set_derogation",
     .fn = HeatingFunction::DEROGATED_SETPOINT,
     .register_low = 0x1D,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::TENTHS_U16_OR_ZERO,
     .write_range = ValueRange{HEATING_TEMP_MIN_C, HEATING_TEMP_MAX_C},
     .action_value_label = "temperature"},
    // Not read on any radiator yet; the same temperature as 0x0137 on a towel dryer, which read it in both.
    {"heater_temperature_in_force", HeatingFunction::HEATER_TEMPERATURE_IN_FORCE, 0x2A, HeatingAccess::READ_ONLY,
     ValueCodec::TENTHS_U16},
    // Night light registers: read on a Sauter Hekla with the light on and off (issue #222).
    {"night_light", HeatingFunction::NIGHT_LIGHT, 0x44, HeatingAccess::READ_WRITE, ValueCodec::BINARY},
    // Written and read as a percentage: the row's scale maps it to the 0-10000 the register holds.
    {.name = "night_light_brightness",
     .fn = HeatingFunction::NIGHT_LIGHT_BRIGHTNESS,
     .register_low = 0x42,
     .access = HeatingAccess::READ_WRITE,
     .codec = ValueCodec::SCALED_U16,
     .max_value = HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX,
     .scale = HEATING_NIGHT_LIGHT_SCALE},
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

/// @brief The value form of a row: its codec and the rule a write must meet.
ValueForm form_of(const HeatingFunctionDescriptor &d) {
  return {.codec = d.codec, .max_value = d.max_value, .write_range = d.write_range, .scale = d.scale};
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

ValueCodec heating_function_codec(HeatingFunction fn) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  return d != nullptr ? d->codec : ValueCodec::NONE;
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

size_t encode_heating_write(HeatingFunction fn, float value, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  // A read-only register is never written, whatever the caller asks: the heater's window sensor and
  // its measurements are not settings.
  if (d == nullptr || d->access == HeatingAccess::READ_ONLY)
    return 0;
  if (d->refuses != nullptr && d->refuses(value))
    return 0;

  // The codec judges the value and lays out its bytes; a refused value emits nothing, never a wrapped byte.
  const size_t value_len =
      encode_value(form_of(*d), value, std::span<uint8_t>(out + PRIVATE_REGISTER_HEADER_SIZE, PAYLOAD_LEN_VALUE_MAX));
  if (value_len == 0)
    return 0;
  write_header(*d, PRIVATE_REGISTER_DIRECTION_SET, out);
  return PRIVATE_REGISTER_HEADER_SIZE + value_len;
}

const char *heating_mode_name(HeatingMode mode) {
  switch (mode) {
    case HeatingMode::AUTO:
      return "auto";
    case HeatingMode::MANUAL:
      return "manual";
    case HeatingMode::PROG:
      return "prog";
    case HeatingMode::EXTERNAL_SETPOINT:
      return "external";
    case HeatingMode::OFF:
      return "off";
  }
  return "unknown";
}

std::optional<HeatingMode> heating_mode_from_name(std::string_view name) {
  for (uint8_t value = 0; value <= HEATING_MODE_MAX; value++) {
    const auto mode = static_cast<HeatingMode>(value);
    if (name == heating_mode_name(mode))
      return mode;
  }
  return std::nullopt;
}

std::string heating_writable_mode_names() {
  std::string names;
  for (uint8_t value = 0; value <= HEATING_MODE_MAX; value++) {
    const auto mode = static_cast<HeatingMode>(value);
    if (!heating_mode_is_writable(mode))
      continue;
    names += names.empty() ? "" : ", ";
    names += heating_mode_name(mode);
  }
  return names;
}

std::optional<HeatingFunction> heating_action_function(std::string_view name) {
  for (const auto &d : HEATING_FUNCTIONS) {
    if (d.action_value_label != nullptr && name == d.name)
      return d.fn;
  }
  return std::nullopt;
}

const char *heating_action_value_label(HeatingFunction fn) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  return d != nullptr ? d->action_value_label : nullptr;
}

bool heating_write_accepts(HeatingFunction fn, float value) {
  uint8_t scratch[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  return encode_heating_write(fn, value, scratch) != 0;
}

std::optional<ValueRange> heating_write_range(HeatingFunction fn) {
  const HeatingFunctionDescriptor *d = descriptor_for(fn);
  return d != nullptr && d->access != HeatingAccess::READ_ONLY ? d->write_range : std::nullopt;
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
      decode_value(form_of(*d), std::span<const uint8_t>(reply.data + PRIVATE_REGISTER_VALUE_OFFSET,
                                                         reply.data_len - PRIVATE_REGISTER_VALUE_OFFSET));
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
