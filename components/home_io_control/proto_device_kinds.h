#pragma once

/// @file proto_device_kinds.h
/// @brief The device kinds: node types, product models, capability classes, cover commands and their names.
/// @ingroup hioc_protocol
///
/// The vocabulary that names what a device is, with no state and no codec behind it. The rows that
/// give each (type, subtype) its names and capabilities live in proto_device_types.h; the runtime
/// device record is in proto_device_model.h.

#include <cstdint>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Device Types — from the IO-Homecontrol specification
// ============================================================================

/// @brief IO-Homecontrol node types.
/// The numeric values follow the specification's 10-bit node-type field, so the enum is 16 bits
/// wide: a remote controller (1023) and the test type (1008) do not fit in 8 bits. Do not reassign
/// or reorder these. Names, subtype names and capabilities live in the table in proto_device_types.h.
enum class DeviceType : uint16_t {
  UNKNOWN = 0,                         ///< Unknown/unspecified device.
  INTERIOR_VENETIAN_BLIND = 1,         ///< Interior venetian blind.
  ROLLER_SHUTTER = 2,                  ///< Roller shutter.
  VERTICAL_EXTERIOR_AWNING = 3,        ///< Vertical exterior awning.
  WINDOW_OPENER = 4,                   ///< Window opening actuator.
  GARAGE_OPENER = 5,                   ///< Garage door opener.
  LIGHT = 6,                           ///< Light.
  GATE_OPENER = 7,                     ///< Gate opener.
  ROLLING_DOOR_OPENER = 8,             ///< Rolling door opener.
  LOCK = 9,                            ///< Lock.
  VERTICAL_INTERIOR_BLIND = 10,        ///< Vertical interior blind.
  SECURE_CONFIGURATION_DEVICE = 11,    ///< Secure configuration device (a commissioning tool, not an actuator).
  BEACON = 12,                         ///< Beacon (unpaired/announcement). Not YAML-selectable: a
                                       ///< discovery pseudo-type, not a configurable actuator.
  DUAL_ROLLER_SHUTTER = 13,            ///< Dual-section roller shutter.
  HEATING_TEMPERATURE_INTERFACE = 14,  ///< Heating temperature interface.
  ON_OFF_SWITCH = 15,                  ///< Generic on/off switch.
  HORIZONTAL_AWNING = 16,              ///< Horizontal awning (open/close inverted).
  EXTERIOR_VENETIAN_BLIND = 17,        ///< Exterior venetian blind.
  LOUVRE_BLIND = 18,                   ///< Louvre blind.
  CURTAIN_TRACK = 19,                  ///< Curtain track.
  VENTILATION_POINT = 20,              ///< Ventilation point.
  EXTERIOR_HEATING = 21,               ///< Exterior heating.
  HEAT_PUMP = 22,                      ///< Heat pump.
  INTRUSION_ALARM = 23,                ///< Intrusion alarm.
  SWINGING_SHUTTER = 24,               ///< Swinging shutter.
  // 25 and 26 are not defined; the enum is non-contiguous from here on.
  SLIDING_WINDOW = 27,             ///< Sliding window.
  ZONE_CONTROL = 28,               ///< Zone control.
  BIOCLIMATIC_PERGOLA = 29,        ///< Bioclimatic pergola (louvre opening as the position).
  SIREN = 30,                      ///< Siren.
  DOMESTIC_HOT_WATER = 51,         ///< Domestic hot water.
  ELECTRICAL_HEATER = 52,          ///< Electrical heater.
  HEAT_RECOVERY_VENTILATION = 53,  ///< Heat recovery ventilation.
  CENTRAL_HOUSE_CONTROL = 255,     ///< Central house control.
  TEST_AND_EVALUATION = 1008,      ///< Test and evaluation equipment.
  REMOTE_CONTROLLER = 1023,        ///< Remote controller (all ten type bits set, packed bytes `FF C0`). An
                                   ///< Atlantic AGILIA IO radiator answers discovery with this type while in
                                   ///< its display's pairing mode, and then refuses a pushed key with
                                   ///< RESULT_COMMAND_UNKNOWN (tests/corpus/captures/pairing/
                                   ///< atlantic_agilia_pairing_key_transfer_command_unknown.yaml).
};

/// @brief A product model below a (node type, subtype): for products that report the same wire type
/// but need different handling.
/// @details The wire carries no model, so it comes from the YAML (`io_model:`) and is never learned.
///          NONE is the (type, subtype) row itself, so every device declared without a model behaves as
///          before. Each other value has a model row in the node-type table (proto_device_types.h) under
///          the one (type, subtype) that offers it; the row's name is the YAML spelling. Values are
///          stable: hub_validators.py `DEVICE_MODEL_OPTIONS` mirrors them (device_features_sync_test.cpp).
enum class DeviceModel : uint8_t {
  NONE = 0,               ///< No model: the (type, subtype) row.
  TOWEL_DRYER = 1,        ///< Towel dryer under the electrical heater with an adjustable setpoint (52/1).
  HEATER_WITH_LIGHT = 2,  ///< Heater with a night light under the electrical heater with an adjustable setpoint (52/1).
  PILOT_WIRE_INTERFACE = 3,   ///< Interface for pilot-wire heaters under the plain electrical heater (52/0): no boost.
  EXPLORER_V4 = 4,            ///< Atlantic Explorer V4 under the domestic hot water heater (51/0).
  ELECTRIC_WATER_HEATER = 5,  ///< Electric storage water heater (Sauter Guelma connecte) under 51/0.
};

/// @brief High‑level capability class derived from DeviceType.
enum class DeviceCapabilityClass : uint8_t {
  UNKNOWN = 0x00,       ///< Unknown capability.
  COVER = 0x01,         ///< Position‑controlled cover (shutter/blind/awning).
  LIGHT = 0x02,         ///< Binary on/off light.
  SWITCH = 0x03,        ///< Binary on/off switch.
  SENSOR = 0x04,        ///< Sensor device.
  BEACON = 0x05,        ///< Beacon.
  CLIMATE = 0x06,       ///< Climate device (heating/cooling).
  LOCK = 0x07,          ///< Lock.
  HEATER_LEVEL = 0x08,  ///< Heater with a heat level in the position field (exterior heating).
  WATER_HEATER = 0x09,  ///< Domestic hot water heater (device_feature::WATER_HEATER).
};

/// @brief Convert a DeviceType to a lowercase string identifier.
/// @param type Device type enum.
/// @return Null‑terminated string name (e.g., "roller_shutter"), or "unknown" for a value the
///         table does not name.
const char *device_type_name(DeviceType type);

/// @brief True when a raw 10-bit device-type value is a named DeviceType enumerator.
/// @details The enum is not contiguous (25 and 26 are undefined), so a range check is not enough.
///          UNKNOWN (0) is not "known".
/// @param raw Raw type value as decoded from an address or info frame.
bool is_known_device_type(uint16_t raw);

/// @brief Return the YAML-friendly device-type name for types exposed in the Python schema.
/// @param type Device type enum.
/// @return Null‑terminated string (e.g., "exterior_venetian_blind"), or nullptr if the type
///         has no symbolic YAML alias (user must use a raw numeric value).
const char *yaml_device_type_name(DeviceType type);

/// @brief Get a human‑readable name for the capability class of a (type, subtype) row.
/// @param type Device type.
/// @param subtype Device subtype; a subtype row can differ from its type's row.
/// @return String like "cover", "light", "switch", "unknown".
const char *device_capability_class_name(DeviceType type, uint8_t subtype);

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
  FORCE_OPEN = 3,  ///< Move to fully open at elevated priority; intended to bypass soft locks
                   ///< and environmental limits (confirmed on real hardware to move correctly;
                   ///< bypassing an active lock is still unconfirmed — see create_force_open()
                   ///< in proto_commands.cpp).
  PARTIAL = 4,     ///< Move to the stored partial position (garage openers; issue #163).
  PEDESTRIAN = 5,  ///< Move to the pedestrian position (gate openers); experimental.
                   ///< \todo Confirm on a real gate opener that 0xD807 opens it to a pedestrian position.
};

/// @brief Get a human-readable name for a CoverCommand.
/// @param cmd The cover command to name.
/// @return Null-terminated string such as "STOP", "FAVORITE", "VENT", "PARTIAL" or "PEDESTRIAN".
const char *cover_command_name(CoverCommand cmd);

}  // namespace home_io_control
}  // namespace esphome
