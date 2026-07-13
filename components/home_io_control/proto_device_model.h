#pragma once

/// @file proto_device_model.h
/// @brief IO-Homecontrol device-type model, capabilities and runtime device state.
/// @ingroup hioc_protocol
///
/// DeviceType/DeviceCapabilityClass, the capability predicates, packed-metadata
/// decoders, cover commands, position/tilt report decoding and the IoDevice
/// runtime struct.

#include "proto_sizes.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Device Types — from the IO-Homecontrol specification
// ============================================================================

/// @brief Device type identifiers reported by IO‑Homecontrol products.
/// The numeric values follow the official specification. Do not reassign or reorder these.
enum class DeviceType : uint8_t {
  UNKNOWN = 0x00,                        ///< Unknown/unspecified device.
  VENETIAN_BLIND = 0x01,                 ///< Venetian blind.
  ROLLER_SHUTTER = 0x02,                 ///< Roller shutter.
  AWNING = 0x03,                         ///< Awning.
  WINDOW_OPENER = 0x04,                  ///< Window opening actuator.
  GARAGE_OPENER = 0x05,                  ///< Garage door opener.
  LIGHT = 0x06,                          ///< Binary light.
  GATE_OPENER = 0x07,                    ///< Gate opener.
  ROLLING_DOOR_OPENER = 0x08,            ///< Rolling door opener.
  LOCK = 0x09,                           ///< Lock.
  BLIND = 0x0A,                          ///< Generic blind.
  SCREEN = 0x0B,                         ///< Insect/privacy screen.
  BEACON = 0x0C,                         ///< Beacon (unpaired/announcement).
  DUAL_SHUTTER = 0x0D,                   ///< Dual-section shutter.
  HEATING_TEMPERATURE_INTERFACE = 0x0E,  ///< Heating temperature interface.
  ON_OFF_SWITCH = 0x0F,                  ///< Generic on/off switch.
  HORIZONTAL_AWNING = 0x10,              ///< Horizontal awning (open/close inverted).
  EXTERNAL_VENETIAN_BLIND = 0x11,        ///< External venetian blind.
  LOUVRE_BLIND = 0x12,                   ///< Louvre blind.
  CURTAIN_TRACK = 0x13,                  ///< Curtain track.
  VENTILATION_POINT = 0x14,              ///< Ventilation point.
  EXTERIOR_HEATING = 0x15,               ///< Exterior heating.
  HEAT_PUMP = 0x16,                      ///< Heat pump.
  INTRUSION_ALARM = 0x17,                ///< Intrusion alarm.
  SWINGING_SHUTTER = 0x18,               ///< Swinging shutter.
};

/// @brief High‑level capability class derived from DeviceType.
enum class DeviceCapabilityClass : uint8_t {
  UNKNOWN = 0x00,  ///< Unknown capability.
  COVER = 0x01,    ///< Position‑controlled cover (shutter/blind/awning).
  LIGHT = 0x02,    ///< Binary on/off light.
  SWITCH = 0x03,   ///< Binary on/off switch.
  SENSOR = 0x04,   ///< Sensor device.
  BEACON = 0x05,   ///< Beacon.
  CLIMATE = 0x06,  ///< Climate device (heating/cooling).
  LOCK = 0x07,     ///< Lock.
};

/// @brief Convert a DeviceType to a lowercase string identifier.
/// @param type Device type enum.
/// @return Null‑terminated string name (e.g., "roller_shutter").
const char *device_type_name(DeviceType type);

/// @brief Return the YAML-friendly device-type name for types exposed in the Python schema.
/// @param type Device type enum.
/// @return Null‑terminated string (e.g., "external_venetian_blind"), or nullptr if the type
///         has no symbolic YAML alias (user must use a raw numeric value).
const char *yaml_device_type_name(DeviceType type);

/// @brief Map a raw IO‑Homecontrol type to the closest ESPHome/Home Assistant entity family.
/// @param type Raw device type.
/// @return Capability class (COVER, LIGHT, SWITCH, etc.).
DeviceCapabilityClass device_capability_class(DeviceType type);

/// @brief Get a human‑readable name for a capability class.
/// @param type Device type (unused, kept for signature compatibility).
/// @return String like "cover", "light", "switch", "unknown".
const char *device_capability_class_name(DeviceType type);

/// @brief Does this device type support precise position control (0–100)?
/// @param type Device type.
/// @return true for cover‑family devices.
bool device_supports_position_control(DeviceType type);

/// @brief Does this device type support binary on/off control?
/// @param type Device type.
/// @return true for lights and switches.
bool device_supports_binary_control(DeviceType type);

/// @brief Does this device type support status request commands (0x03)?
/// @param type Device type.
/// @return true for covers, binary devices, and lock devices.
bool device_supports_status_requests(DeviceType type);

/// @brief Does this device type support binary lock/unlock control via execute commands?
/// @param type Device type.
/// @return true for lock devices.
bool device_supports_lock_control(DeviceType type);

/// @brief Does this device type support tilt (slat angle) control?
/// @param type Device type.
/// @return true for venetian blinds, blinds, external venetian blinds, louvre blinds.
bool device_supports_tilt(DeviceType type);

/// @brief Does this device type support the ventilation position command?
///
/// The ventilation command moves window-type actuators to a predefined
/// partially-open position suitable for air exchange without fully opening.
/// @param type Device type.
/// @return true for WINDOW_OPENER and VENTILATION_POINT.
bool device_supports_vent(DeviceType type);

/// @brief Decode a protocol-packed device type from two metadata bytes.
/// @param type_msb First metadata byte.
/// @param type_subtype Second metadata byte containing the remaining type bits and subtype.
/// @return Decoded device type.
DeviceType decode_packed_device_type(uint8_t type_msb, uint8_t type_subtype);

/// @brief Decode a protocol-packed device subtype from the second metadata byte.
/// @param type_subtype Second metadata byte containing subtype in bits [5:0].
/// @return Manufacturer-specific subtype.
uint8_t decode_packed_device_subtype(uint8_t type_subtype);

/// @brief Human‑readable operation profile name for a device type.
/// Used for logging and diagnostics.
/// @param type Device type.
/// @return String such as "cover_position", "cover_position_tilt", "binary_on_off", "lock", etc.
const char *device_operation_profile_name(DeviceType type);

// ============================================================================
// Cover Commands
// ============================================================================

/// @brief Named device commands for cover-type actuators.
///
/// These represent the discrete non-positional actions a controller can send to
/// a cover device. Each maps to a specific wire encoding in the CMD_EXECUTE payload.
/// Using this enum avoids conflating numeric positions (0–100) with command codes.
enum class CoverCommand : uint8_t {
  STOP = 0,        ///< Stop movement immediately.
  FAVORITE = 1,    ///< Move to stored favorite/"My" position.
  VENT = 2,        ///< Move to ventilation position (window-type devices).
  FORCE_OPEN = 3,  ///< Force fully open, bypassing soft locks and environmental limits.
};

/// @brief Get a human-readable name for a CoverCommand.
/// @param cmd The cover command to name.
/// @return Null-terminated string such as "STOP", "FAVORITE", or "VENT".
const char *cover_command_name(CoverCommand cmd);

// ============================================================================
// Position Report Decoding
// ============================================================================

/// In status responses, position is encoded as a 16-bit value where
/// 0x0000 = fully open (0%) and 0xC800 = fully closed (100%).
static constexpr uint16_t STATUS_POS_MAX = 0xC800;
/// Target-reached tolerance expressed in raw IO-homecontrol position units.
/// 100 raw units out of 51200 full-scale is about 0.195%, so this only absorbs
/// tiny target/current mismatches from device rounding or early stopped flags.
static constexpr uint16_t STATUS_POS_TOLERANCE_RAW = 100;

/// Packed device metadata uses two bytes where the high 8 bits carry the upper type bits and the
/// low byte carries both the remaining type bits and the 6-bit manufacturer subtype.
static constexpr uint8_t DEVICE_METADATA_SIZE = 2;
static constexpr uint8_t DEVICE_TYPE_LOW_BITS_SHIFT = 2;
static constexpr uint8_t DEVICE_TYPE_HIGH_BITS_SHIFT = 6;
static constexpr uint8_t DEVICE_SUBTYPE_MASK = 0x3F;

// ============================================================================
// Device State
// ============================================================================

/// @brief Sentinel value meaning "position is not known yet".
/// Matches POS_UNKNOWN (0xD4 = 212 decimal) for easy debugging.
static constexpr float UNKNOWN_POSITION = 212.0F;
static constexpr uint8_t DEVICE_NAME_BUFFER_SIZE = 32;  ///< Device name storage including null terminator

/// @brief Runtime state of a paired IO‑Homecontrol device.
struct IoDevice {
  uint8_t node_id[NODE_ID_SIZE]{};       ///< Device's 3‑byte radio address.
  DeviceType type{DeviceType::UNKNOWN};  ///< Device type (shutter, awning, etc.).
  uint8_t subtype{0};                    ///< Device subtype (manufacturer‑specific).
  char name[DEVICE_NAME_BUFFER_SIZE]{};  ///< Cached UTF-8 device name decoded from Latin-1 wire payloads.
  float position{UNKNOWN_POSITION};      ///< Current position: 0=open, 100=closed, or UNKNOWN_POSITION.
  float tilt{UNKNOWN_POSITION};          ///< Current tilt: 0=closed, 100=open, or UNKNOWN_POSITION.
  float target{UNKNOWN_POSITION};        ///< Target position the device is moving toward.
  bool is_stopped{true};                 ///< True if device is not moving.
  bool inverted{false};                  ///< True if open/close positions are swapped (e.g., horizontal awning).
  bool optimistic_state{true};           ///< True if `target` may be set ahead of a confirming poll/response.
  uint8_t last_result_code{0};           ///< Last CMD_ERROR_RESP result byte (0 = none recorded). See
                                ///< command_result_name()/is_limitation_result() in proto_constants.h. Cleared by
                                ///< the next successful status/command reply for this device. Note: 0 is also the
                                ///< real RESULT_UNKNOWN_STATUS_REPLY wire value, so that specific explicit reply is
                                ///< indistinguishable here from "nothing recorded yet" — a known, accepted tradeoff.
  uint32_t last_result_at_ms{0};  ///< millis() timestamp of last_result_code, 0 when none recorded.
  uint32_t last_status{0};        ///< millis() timestamp of last received status.
};

/// @brief Determine whether a device type has inverted position mapping by default.
/// @param type Device type.
/// @return true for horizontal awnings; false otherwise.
bool default_inverted_for_type(DeviceType type);

/// @brief Decode target/current position values from a status frame.
/// @param target_raw 16‑bit raw target value.
/// @param current_raw 16‑bit raw current value.
/// @param is_stopped True if device reports stopped.
/// @param target Output target position (0–100 or UNKNOWN_POSITION).
/// @param position Output current position (0–100 or UNKNOWN_POSITION).
void decode_position_report(uint16_t target_raw, uint16_t current_raw, bool is_stopped, float &target, float &position);
/// @brief Has the device reached its target within tolerance?
/// @param target Target position (0–100 or UNKNOWN_POSITION).
/// @param position Current position (0–100 or UNKNOWN_POSITION).
/// @return true if positions match within STATUS_POS_TOLERANCE_RAW.
bool has_reached_target_position(float target, float position);
/// @brief Decode tilt angle from raw 16‑bit value.
/// @param tilt_raw Raw tilt value from status frame.
/// @return Tilt percentage (0 = closed, 100 = open) or UNKNOWN_POSITION.
float decode_tilt_report(uint16_t tilt_raw);

}  // namespace home_io_control
}  // namespace esphome
