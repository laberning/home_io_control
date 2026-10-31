#pragma once

/// @file proto_heating.h
/// @brief Pure codec for the private register protocol of 2W heaters (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_protocol
///
/// A heater with an adjustable setpoint (node type 52, subtype 1) exposes its state as 16-bit
/// registers `0x01xx`, read with `0C 60 01 <reg>` and written with `0C 61 01 <reg> <value>`; the
/// device answers with CMD_WRITE_PRIVATE_ACK (0x21) echoing the header, followed by the value. Values
/// are little-endian. An Atlantic AGILIA IO radiator answers every read and write this way
/// (tests/corpus/captures/exchange/atlantic_agilia_exchange_{status_read,setpoint_and_off_read_back,mode_read_back}.yaml,
/// pinned by corpus_heating_test.cpp); see also
/// tests/corpus/captures/exchange/atlantic_thermor_exchange_write_private_param.yaml.
///
/// One descriptor table (`HEATING_FUNCTIONS` in the .cpp) lists every register this component
/// uses: its access (read-only, read/write, write-only) and how its value is encoded. The same row
/// drives the get payload, the set payload and the decoder of the reply, so a register is described
/// exactly once. This module is pure: it does not touch a device record or the radio —
/// proto_commands.h's create_write_private() frames a payload.
///
/// Confirmed on that radiator: the setpoint, mode (all four writable values), room temperature,
/// regulator and maximum-setpoint registers. The presence register has been written and acknowledged,
/// with no visible effect observed; the window and open-window-detection registers have only been read
/// at rest.

#include "proto_frame.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Registers and functions of a 2W heater, one row of the descriptor table each.
///
/// The `SET_*` names are the functions of the `heating_control` action; the same row is also the
/// register's read path (reading SET_TEMPERATURE reads the comfort setpoint).
enum class HeatingFunction : uint8_t {
  POWER_ON,                   ///< Register 0x012C, read: the paired-device list. Logged, never decoded.
  SET_TEMPERATURE,            ///< Register 0x0103, read/write: comfort setpoint, tenths of a degree.
  SET_MODE,                   ///< Register 0x0100, read/write: operating mode (HeatingMode).
  SET_PRESENCE,               ///< Register 0x0110, write-only: someone is in the room (1) or not (0).
  SET_OPEN_WINDOW_DETECTION,  ///< Register 0x0106, read/write: open-window detection on (1) or off (0).
  MIDNIGHT_SYNC,              ///< Register 0x0130, read: the comfort/eco/auto setpoint block. Logged, never decoded.
  ROOM_TEMPERATURE,           ///< Register 0x0115, read-only: measured room temperature, signed hundredths of a degree.
  REGULATOR_STATE,            ///< Register 0x0117, read-only: 0 none, 1 standby, 2 heating.
  MAX_SETPOINT,               ///< Register 0x0102, read-only: highest setpoint the heater accepts, tenths of a degree.
  WINDOW_OPEN,                ///< Register 0x010E, read-only: the heater's window sensor, 0 closed / 1 open.
};

/// @brief Operating modes of the mode register (HeatingFunction::SET_MODE).
enum class HeatingMode : uint8_t {
  AUTO = 0x00,               ///< Automatic mode; the device manages the setpoint itself.
  MANUAL = 0x01,             ///< Manual mode; follows the last written setpoint.
  PROG = 0x02,               ///< Program mode; the device runs its own stored weekly schedule.
  EXTERNAL_SETPOINT = 0x03,  ///< Setpoint comes from an external controller. Readable, never written by this
                             ///< component. Not named EXTERNAL: the Arduino core defines that as a macro.
  OFF = 0x04,                ///< Off / standby.
};

/// @brief Regulator states of the regulator register (HeatingFunction::REGULATOR_STATE).
enum class HeatingRegulatorState : uint8_t {
  NONE = 0x00,     ///< No regulation active.
  STANDBY = 0x01,  ///< Regulating, the element is idle.
  HEATING = 0x02,  ///< The element is heating.
};

/// @brief Lowest setpoint this codec will encode.
///
/// The conventional frost-protection setpoint, safely representable as wire value 70.
constexpr float HEATING_TEMP_MIN_C = 7.0F;

/// @brief Highest setpoint this codec will encode.
///
/// The setpoint register is a 16-bit little-endian value in tenths of a degree; 28.0 °C (`18 01`) is
/// the ceiling Atlantic radiator manuals document and the value an AGILIA IO radiator reports in its
/// maximum setpoint register (tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml).
/// No setpoint above 25.5 °C (a non-zero high byte) has been written on hardware yet.
constexpr float HEATING_TEMP_MAX_C = 28.0F;

/// @brief Largest payload any function produces: a set of a 16-bit value (4-byte header + 2 value bytes).
constexpr size_t HEATING_PAYLOAD_MAX_SIZE = 6;

/// @brief Leading payload byte, common to every function. Its meaning is unconfirmed; do not read a
/// semantic (for example "device class") into it.
constexpr uint8_t HEATING_PAYLOAD_PREFIX = 0x0C;

/// @brief Payload byte at index 2: the high byte of the 16-bit register number, constant `0x01`.
constexpr uint8_t HEATING_REGISTER_HIGH_BYTE = 0x01;

/// @brief Payload byte at index 1 of a read ("get") request.
constexpr uint8_t HEATING_DIRECTION_GET = 0x60;

/// @brief Payload byte at index 1 of a write ("set") request.
constexpr uint8_t HEATING_DIRECTION_SET = 0x61;

/// @brief Measured value in a reading when the heater reports a sensor fault (raw `-1`).
/// A reading stores it as NaN, the same as "never read".
constexpr int16_t HEATING_SENSOR_FAULT_RAW = -1;

/// @brief Default encoding range of a temperature main parameter, in degrees Celsius. The main
/// parameter spans HEATING_TEMPERATURE_MP_SPAN steps over [min, max]; this range is the one a device
/// is assumed to use unless the YAML says otherwise (`min_temperature:` / `max_temperature:`).
static constexpr float HEATING_TEMPERATURE_RANGE_MIN_C = -40.0F;
static constexpr float HEATING_TEMPERATURE_RANGE_MAX_C = 80.0F;  ///< See HEATING_TEMPERATURE_RANGE_MIN_C.

/// @brief Stable lowercase name for a heating function ("power_on", "set_temperature", ...).
/// @param fn Function.
/// @return Null-terminated string; "unknown" for an out-of-range value.
const char *heating_function_name(HeatingFunction fn);

/// @brief Whether the register behind a function can be written.
/// @param fn Function.
/// @return true for a read/write or write-only register; false for a read-only one or an
///         out-of-range value.
bool heating_function_is_writable(HeatingFunction fn);

/// @brief Whether the register behind a function can be read.
/// @param fn Function.
/// @return true for a read/write or read-only register; false for a write-only one or an
///         out-of-range value.
bool heating_function_is_readable(HeatingFunction fn);

/// @brief Encode a write ("set") of one register into a CMD_WRITE_PRIVATE (0x20) payload.
///
/// `value` is interpreted by the register's encoding:
///   - SET_TEMPERATURE: degrees Celsius within [HEATING_TEMP_MIN_C, HEATING_TEMP_MAX_C], encoded as
///     a 16-bit little-endian value in tenths of a degree, round-half-away-from-zero(10 * value).
///   - SET_MODE: a HeatingMode value widened to float; must be AUTO, MANUAL, PROG or OFF.
///   - SET_PRESENCE, SET_OPEN_WINDOW_DETECTION: 0 or 1.
/// A read-only register (including POWER_ON, MIDNIGHT_SYNC and the measurement registers) is never
/// encoded as a write, and out-of-range, non-integral (for enum/binary encodings) or non-finite
/// input is rejected: a byte derived from a truncating or wrapping cast is never emitted.
/// @param fn Register to write.
/// @param value Register-specific value (see above).
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return Number of payload bytes written (5 or 6), or 0 on invalid input or a read-only register.
size_t encode_heating_write(HeatingFunction fn, float value, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

/// @brief Encode a read ("get") of one register into a CMD_WRITE_PRIVATE (0x20) payload.
/// @param fn Register to read.
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return 4 (the payload is always `0C 60 01 <reg>`), or 0 for a write-only register or an
///         out-of-range value.
size_t encode_heating_read(HeatingFunction fn, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

/// @brief Encode the function the `heating_control` action runs: a read for a read-only register,
/// a write for every other.
///
/// POWER_ON and MIDNIGHT_SYNC are reads (their `value` is ignored); the measurement registers are
/// reads too; the others are writes as in encode_heating_write().
/// @param fn Function to encode.
/// @param value Function-specific value (ignored for a read).
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return Number of payload bytes written (4, 5 or 6), or 0 on invalid input.
size_t encode_heating_payload(HeatingFunction fn, float value, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

/// @brief Decode the value a heater answered a register read or write with.
///
/// Accepts a CMD_WRITE_PRIVATE_ACK (0x21) whose data starts with `0C 60 01 <reg>` or
/// `0C 61 01 <reg>` for this function's register, followed by at least the register's value bytes.
/// Anything else (another command, another register, a truncated value, a function without a typed
/// decode such as POWER_ON) is "no value".
///
/// The value is returned in natural units: degrees Celsius for the temperature registers, the raw
/// byte for the others. A raw `-1` on the room-temperature register is a sensor fault: it decodes
/// as NaN with `true` returned, because the heater did answer.
/// @param reply The device's reply frame.
/// @param fn Register the request addressed.
/// @param[out] value Decoded value; untouched when `false` is returned.
/// @return true when the reply carries a value for this register.
bool decode_heating_reply(const IoFrame &reply, HeatingFunction fn, float &value);

/// @brief What the hub has read from one heater so far.
///
/// Every field starts as "never read" (NaN, or -1 for the byte registers). Only a decoded reply
/// changes a field, so what an entity shows from here is observed, never predicted.
struct HeatingReadings {
  float room_temperature_c{NAN};      ///< Measured room temperature; NaN until read or on a sensor fault.
  float setpoint_c{NAN};              ///< Comfort setpoint as the heater holds it; NaN until read.
  float max_setpoint_c{NAN};          ///< Highest setpoint the heater accepts; NaN until read.
  int16_t mode{-1};                   ///< HeatingMode value; -1 until read.
  int16_t regulator{-1};              ///< HeatingRegulatorState value; -1 until read.
  int16_t window_open{-1};            ///< 0 closed / 1 open; -1 until read.
  int16_t open_window_detection{-1};  ///< 1 detection on / 0 off; -1 until read.
  int16_t level{-1};                  ///< Low byte of the main parameter of a heater commanded with a prefixed
                                      ///< value: a heating level, or a setpoint alias. -1 until a status reply
                                      ///< carried one, and again after a reply that carried a plain
                                      ///< temperature. Set by the hub's status path, not by store().
  int32_t main_parameter{-1};         ///< The raw current main parameter of a heater commanded with an EXECUTE
                                      ///< (a temperature, an alias or a level); -1 until a status reply
                                      ///< carried one. Set by the hub's status path, not by store().

  /// @brief Fold one decoded register value into the readings.
  /// @param fn Register the value was read from.
  /// @param value Value as returned by decode_heating_reply().
  /// @return true when `fn` is a register the readings hold (and the field was set).
  bool store(HeatingFunction fn, float value);

  /// @brief Has this register been read yet?
  /// @param fn Register.
  /// @return true when the readings hold a value for `fn`; false for "never read" and for a function
  ///         the readings do not hold.
  [[nodiscard]] bool has(HeatingFunction fn) const;
};

}  // namespace home_io_control
}  // namespace esphome
