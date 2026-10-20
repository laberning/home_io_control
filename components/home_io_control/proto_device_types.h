#pragma once

/// @file proto_device_types.h
/// @brief The IO node-type and subtype table: names, entity class, platform and behaviour flags.
/// @ingroup hioc_protocol
///
/// One row per (node type, subtype) the project knows by name. Every per-type fact that is not a
/// bare number — the lowercase identifier, the YAML selectability, the capability class, whether a
/// platform can drive the device yet, and the behaviour flags — is read from that table, so there is
/// exactly one place to add a type or a subtype. A lookup falls back from (type, subtype) to
/// (type, 0) to "not in the table".
///
/// Subtype names agree across manufacturers, so rows are keyed on (type, subtype) only. A row names
/// a wire type, not a product: products that share a (type, subtype) are told apart by data the wire
/// does not carry, which is why the names stay generic.

#include "proto_device_model.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Subtype value that marks a device taking the main parameter only, on any node type.
/// The subtype is defined that way in the public VELUX KLF 200 API specification (Appendix 2).
static constexpr uint8_t SUBTYPE_MAIN_PARAMETER_ONLY = 58;

/// @brief Bit flags carried by a DeviceTypeInfo row.
namespace device_type_flags {
/// Type rows only: the type's identifier is accepted by the YAML `io_device_type` key
/// (hub_validators.py `DEVICE_TYPE_OPTIONS` lists exactly these).
static constexpr uint8_t YAML_SELECTABLE = 1U << 0U;
/// An ESPHome platform of the row's capability class can drive this (type, subtype). Rows without
/// it are reported as "not supported yet" and get no paste-ready YAML.
static constexpr uint8_t HAS_PLATFORM = 1U << 1U;
/// Movement commands carry the main parameter only (no functional-parameter block), see
/// device_takes_main_parameter_only().
static constexpr uint8_t MAIN_PARAMETER_ONLY = 1U << 2U;
/// Light rows only: brightness is the only controllable property (no colour, no colour temperature).
static constexpr uint8_t BRIGHTNESS_ONLY = 1U << 3U;
/// Subtype rows only: `name` already is the complete description ("window lock") and is printed
/// alone, instead of being appended to the type's name ("roller shutter, with projection").
static constexpr uint8_t STANDALONE_NAME = 1U << 4U;
}  // namespace device_type_flags

/// @brief One row of the node-type / subtype table.
struct DeviceTypeInfo {
  DeviceType type;                   ///< Node type.
  uint8_t subtype;                   ///< Subtype; 0 is the type's own row.
  const char *name;                  ///< Type row: lowercase identifier ("roller_shutter"). Subtype row:
                                     ///< a description that follows the type name ("with projection").
  DeviceCapabilityClass capability;  ///< Entity family the device maps to.
  uint8_t flags;                     ///< device_type_flags bits.
};

/// @brief Look up the table row for a (type, subtype) pair.
/// @param type Node type.
/// @param subtype Subtype; a subtype with no row of its own falls back to the type's row (subtype 0).
/// @return The matching row, or nullptr when the type is not in the table.
const DeviceTypeInfo *find_device_type_info(DeviceType type, uint8_t subtype);

/// @brief Look up the table row with exactly this (type, subtype), without falling back.
/// @param type Node type.
/// @param subtype Subtype.
/// @return The row, or nullptr when there is none.
const DeviceTypeInfo *find_exact_device_type_info(DeviceType type, uint8_t subtype);

/// @brief Can an ESPHome platform drive this (type, subtype) today?
/// @param type Node type.
/// @param subtype Subtype.
/// @return true when the resolved row carries device_type_flags::HAS_PLATFORM.
bool device_has_platform(DeviceType type, uint8_t subtype);

/// @brief Name of the ESPHome platform that drives this (type, subtype).
/// @param type Node type.
/// @param subtype Subtype.
/// @return "cover", "light", "switch", "lock", "climate" or "fan", or nullptr when no platform drives it yet.
const char *device_platform_name(DeviceType type, uint8_t subtype);

/// @brief Capability class of a (type, subtype), subtype-aware.
/// @details device_capability_class(DeviceType) answers for the type alone; this overload lets a
///          subtype row differ from its type (for example a heater with an adjustable setpoint).
/// @param type Node type.
/// @param subtype Subtype.
/// @return The resolved row's class, or DeviceCapabilityClass::UNKNOWN when the type is not in the table.
DeviceCapabilityClass device_capability_class(DeviceType type, uint8_t subtype);

/// @brief Is this light limited to brightness (no colour, no colour temperature)?
/// @param type Node type.
/// @param subtype Subtype.
/// @return true when the resolved row carries device_type_flags::BRIGHTNESS_ONLY.
bool device_light_is_brightness_only(DeviceType type, uint8_t subtype);

/// @brief Human-readable description of a (type, subtype) for logs and reports.
/// @param type Node type.
/// @param subtype Subtype.
/// @return "<type name>, <subtype name>" (for example "electrical heater, with adjustable setpoint"),
///         "<type name>, subtype N" when the subtype has no name, the bare type name for subtype 0,
///         and "unknown type 0xNN" when the type is not in the table.
std::string format_device_kind(DeviceType type, uint8_t subtype);

/// @brief Wire values of a (type, subtype), for logs that must stay readable without the table.
/// @param type Node type.
/// @param subtype Subtype.
/// @return "0xTT/S", for example "0x34/1".
std::string format_device_ids(DeviceType type, uint8_t subtype);

}  // namespace home_io_control
}  // namespace esphome
