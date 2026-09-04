#pragma once

/// @file proto_heating.h
/// @brief Pure codec for IO-Homecontrol 2W heating/climate functions (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_protocol
///
/// Encodes the six "Cozytouch" radiator functions (Sauter / Atlantic / Thermor) into
/// CMD_WRITE_PRIVATE (0x20) payloads. This module is pure: it does not touch a frame, a device
/// record or the radio — proto_commands.h's create_write_private() frames the result.
///
/// Byte-layout sources: the iohomecontrol reference implementation's Cozy 2W device code —
/// `forgePacket()` and the DeviceButton payload builders in its `cmd()` — cross-checked against
/// the iown-homecontrol project's Atlantic/Thermor register map, which names the
/// opcodes ("Set 0c61 01xx" / "Get 0c60 01xx") and shows the setpoint registers as 16-bit
/// little-endian tenths of a degree. Every payload byte below is cited. No hardware is available
/// for this family; the experimental banner lives in docs/home_io_control.md.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Heating functions, one per user-pressable radiator button in the reference.
enum class HeatingFunction : uint8_t {
  POWER_ON,         ///< Wake / retrieve paired devices (iohcCozyDevice2W.cpp:105).
  SET_TEMPERATURE,  ///< Setpoint in degrees Celsius (iohcCozyDevice2W.cpp:125).
  SET_MODE,         ///< Operating mode (iohcCozyDevice2W.cpp:155).
  SET_PRESENCE,     ///< Presence / absence (iohcCozyDevice2W.cpp:195).
  SET_WINDOW,       ///< Open-window / frost-protection (iohcCozyDevice2W.cpp:218).
  MIDNIGHT_SYNC,    ///< Reads register 0x0130 — the comfort/eco/auto setpoint block
                    ///< (iohcCozyDevice2W.cpp:248; AtlanticThermor/README.md's 0x0130 rows). Named
                    ///< "midnight" in the reference, but the payload is a 0x60 *read*, not a
                    ///< clock-set: the device's clock register is 0x010F and this component never
                    ///< writes it. Provided for protocol exploration; the 0x21 ACK payload is
                    ///< logged at DEBUG (see IOHomeControlComponent::send_heating_command()).
};

/// @brief Operating modes for HeatingFunction::SET_MODE.
///
/// Values from iohcCozyDevice2W.cpp:158-162. The 0x03 "special" mode is commented out at :161 in
/// the reference and is deliberately not exposed here.
enum class HeatingMode : uint8_t {
  AUTO = 0x00,    ///< Automatic mode; device manages the setpoint itself (iohcCozyDevice2W.cpp:158).
  MANUAL = 0x01,  ///< Manual mode; follows the last SET_TEMPERATURE setpoint (iohcCozyDevice2W.cpp:159).
  PROG = 0x02,    ///< Program mode; device runs its own stored weekly schedule (iohcCozyDevice2W.cpp:160).
  OFF = 0x04,     ///< Off / standby; note the value is 0x04, not 0x03 (that is the reference's
                  ///< commented-out "special" mode) (iohcCozyDevice2W.cpp:162).
};

/// @brief Lowest setpoint this codec will encode.
///
/// The conventional frost-protection setpoint. No reference evidence pins a hard lower bound;
/// 7.0 is safely representable as wire byte 0x46.
constexpr float HEATING_TEMP_MIN_C = 7.0F;

/// @brief Highest setpoint this codec will encode.
///
/// iohcCozyDevice2W.cpp:125-128 only writes the low byte of the setpoint field, so *that*
/// implementation tops out at 0xFF = 25.5 C — a latent bug, not the wire limit. The vendored
/// Atlantic register map (AtlanticThermor/README.md) shows the setpoint field is a 16-bit
/// little-endian value in tenths of a degree: its 0x0130 block carries `18 01` = 0x0118 = 280 =
/// 28.0 C as a live setpoint. This codec writes both bytes and allows up to 28.0 C, the ceiling
/// Atlantic radiator manuals document. The encoding above 25.5 C is corroborated by the vendored
/// register map but still unverified on hardware — see docs/home_io_control.md.
constexpr float HEATING_TEMP_MAX_C = 28.0F;

/// @brief Largest payload any function produces — SET_TEMPERATURE's 6-byte form
/// (iohcCozyDevice2W.cpp:125).
constexpr size_t HEATING_PAYLOAD_MAX_SIZE = 6;

/// @brief Leading payload byte, common to every function (iohcCozyDevice2W.cpp:105 et al.).
///
/// The reference gives it no name and no explanation; its meaning is unconfirmed. Do not read a
/// semantic (e.g. "device class") into it.
constexpr uint8_t HEATING_PAYLOAD_PREFIX = 0x0C;

/// @brief Payload byte at index 2 — the high byte of the 16-bit register number, constant `0x01`
/// (registers are `0x01xx`) per AtlanticThermor/README.md ("Set 0c61 01xx" / "Get 0c60 01xx").
/// The low byte (register selector) is per-function; see HeatingFunctionDescriptor::register_low.
constexpr uint8_t HEATING_REGISTER_HIGH_BYTE = 0x01;

/// @brief Stable lowercase name for a heating function ("power_on", "set_temperature", ...).
/// @param fn Function.
/// @return Null-terminated string; "unknown" for an out-of-range value.
const char *heating_function_name(HeatingFunction fn);

/// @brief Encode one heating function into a CMD_WRITE_PRIVATE (0x20) payload.
///
/// Table-driven: a single constexpr descriptor table supplies the direction byte, register-low
/// byte and value kind per function, so there is no copy-pasted per-function builder. `value` is
/// interpreted by function:
///   - SET_TEMPERATURE: degrees Celsius, must lie within
///     [HEATING_TEMP_MIN_C, HEATING_TEMP_MAX_C]; encoded as a 16-bit little-endian value in tenths
///     of a degree, round-half-away-from-zero(10 * value) (AtlanticThermor/README.md's 0x0103 /
///     0x0130 rows). iohcCozyDevice2W.cpp:125-128 only writes the low byte and truncates; this
///     codec writes both bytes and rounds, deliberately.
///   - SET_MODE: a HeatingMode value widened to float (e.g. `float(HeatingMode::MANUAL)`); must
///     be exactly one of AUTO / MANUAL / PROG / OFF.
///   - SET_PRESENCE: 0 (absent) or 1 (present) (iohcCozyDevice2W.cpp:198-199).
///   - SET_WINDOW: 0 (closed) or 1 (open / frost protection) (iohcCozyDevice2W.cpp:221-222).
///   - POWER_ON / MIDNIGHT_SYNC: `value` is ignored.
/// Out-of-range, non-integral (for enum/binary kinds) or non-finite input is rejected with a 0
/// return — a byte derived from a truncating or wrapping cast is never emitted.
/// @param fn Function to encode.
/// @param value Function-specific value (see above).
/// @param out Output buffer of HEATING_PAYLOAD_MAX_SIZE bytes.
/// @return Number of payload bytes written (4, 5 or 6), or 0 on invalid input.
size_t encode_heating_payload(HeatingFunction fn, float value, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]);

}  // namespace home_io_control
}  // namespace esphome
