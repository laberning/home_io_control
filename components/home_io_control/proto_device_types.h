#pragma once

/// @file proto_device_types.h
/// @brief The IO node-type and subtype table: names, entity class, platform and behaviour flags.
/// @ingroup hioc_protocol
///
/// One row per (node type, subtype) the project knows by name. Every per-type fact that is not a
/// bare number — the lowercase identifier, the YAML selectability, the capability class, whether a
/// platform can drive the device yet, and the capability bits — is read from that table, so there is
/// exactly one place to add a type or a subtype. A lookup falls back from (type, subtype) to
/// (type, 0) to "not in the table".
///
/// Subtype names agree across manufacturers, so rows are keyed on (type, subtype) only. A row names
/// a wire type, not a product: products that share a (type, subtype) are told apart by data the wire
/// does not carry, which is why the names stay generic.

#include "proto_constants.h"
#include "proto_device_features.h"
#include "proto_device_model.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Subtype value that marks a device taking the main parameter only, on any node type.
/// The subtype is defined that way in the public VELUX KLF 200 API specification (Appendix 2).
static constexpr uint8_t SUBTYPE_MAIN_PARAMETER_ONLY = 58;

/// @brief Subtype of the electrical heater (node type 52) with an adjustable setpoint, the one driven
/// over the private register protocol (proto_heating.h).
static constexpr uint8_t SUBTYPE_HEATER_WITH_SETPOINT = 1;

/// @brief Bit flags carried by a DeviceTypeInfo row: identity facts. Capabilities are DeviceFeatures.
namespace device_type_flags {
/// Type rows only: the type's identifier is accepted by the YAML `io_device_type` key
/// (hub_validators.py `DEVICE_TYPE_OPTIONS` lists exactly these).
static constexpr uint8_t YAML_SELECTABLE = 1U << 0U;
/// An ESPHome platform of the row's capability class can drive this (type, subtype). Rows without
/// it are reported as "not supported yet" and get no paste-ready YAML.
static constexpr uint8_t HAS_PLATFORM = 1U << 1U;
/// Subtype rows only: `name` already is the complete description ("window lock") and is printed
/// alone, instead of being appended to the type's name ("roller shutter, with projection").
static constexpr uint8_t STANDALONE_NAME = 1U << 2U;
}  // namespace device_type_flags

/// @brief One row of the node-type / subtype table.
struct DeviceTypeInfo {
  DeviceType type;                   ///< Node type.
  uint8_t subtype;                   ///< Subtype; 0 is the type's own row.
  const char *name;                  ///< Type row: lowercase identifier ("roller_shutter"). Subtype row:
                                     ///< a description that follows the type name ("with projection").
  DeviceCapabilityClass capability;  ///< Entity family the device maps to.
  uint8_t flags;                     ///< device_type_flags bits.
  DeviceFeatures features;           ///< device_feature bits: the whole word, never merged with the type row's.
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

/// @brief Capability bits of a (type, subtype).
/// @details The exact row; else, for subtype SUBTYPE_MAIN_PARAMETER_ONLY, main_parameter_only_variant()
///          of the type's row (of no features when the type has no row); else the type's own row; else
///          no features. Callers resolve a device's bits once, onto the device record, and read them
///          from there; this lookup serves the resolver and checks that run before a device exists.
/// @param type Node type.
/// @param subtype Subtype.
/// @return The feature word.
DeviceFeatures device_features(DeviceType type, uint8_t subtype);

/// @brief Features of a device kind that has no table row, or that is not declared at all.
/// @details An undeclared device keeps the permissive behaviour its entity gives it until the type is
///          learned and a row takes over: a cover accepts a slider, favourite and vent and sends the
///          8-byte position frame; a light, switch or lock sends the 8-byte frame; an exterior-heating
///          fan takes levels in that frame. No class has tilt.
/// @param entity_class Capability class of the entity that registered the device.
/// @return The feature word, or no features for a class with no EXECUTE position path.
DeviceFeatures undeclared_kind_features(DeviceCapabilityClass entity_class);

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

/// @brief The features of a subtype-58 variant of a kind.
/// @details VELUX KLF 200 API specification Appendix 2 defines subtype 58 as "main parameter only" for
///          a few node types; this component applies the same rule to every type and, from issue #163,
///          also reads it as "no intermediate values". The variant clears LEVELS, TILT and
///          EXTENDED_POSITION_FRAME (each needs a functional parameter or an intermediate value) and sets
///          MAIN_PARAMETER_ONLY. Named positions are main-parameter values of the family's actuator
///          profile (KLF 200 section 14.2) and are kept. Used by the 58 rows and by the lookup fallback.
/// @param type_features The features of the kind the variant derives from.
/// @return The variant's features.
constexpr DeviceFeatures main_parameter_only_variant(DeviceFeatures type_features) {
  using namespace device_feature;
  return static_cast<DeviceFeatures>((type_features & ~(LEVELS | TILT | EXTENDED_POSITION_FRAME)) |
                                     MAIN_PARAMETER_ONLY);
}

// === Main-parameter values ===

/// @brief Combine the two bytes of a main parameter as the EXECUTE payload carries them.
/// @param high First (high) byte.
/// @param low Second (low) byte, the modifier of a named position.
constexpr uint16_t main_parameter_of(uint8_t high, uint8_t low = 0) {
  return static_cast<uint16_t>((static_cast<uint16_t>(high) << BITS_PER_BYTE) | low);
}

/// Main parameter that opens (or switches off) a device: percent 0.
static constexpr uint16_t MAIN_PARAMETER_OPEN = 0x0000;
/// Main parameter that closes (or switches on) a device: 100 percent, the top of the position scale.
static constexpr uint16_t MAIN_PARAMETER_CLOSED = STATUS_POS_MAX;
/// Main parameter that stops a movement.
static constexpr uint16_t MAIN_PARAMETER_STOP = main_parameter_of(POS_STOP);
/// What position_main_parameter() returns for a value that is no position; no device accepts it.
static constexpr uint16_t MAIN_PARAMETER_INVALID = 0xFFFF;

/// @brief One named position: its feature bit, its main-parameter value and the command that sends it.
/// @details Named positions are main-parameter values defined per actuator profile (VELUX KLF 200 API
///          specification, section 14.2). NAMED_POSITIONS is the only place a value, its bit and its
///          command are paired; the frame builder, the value check, the command names and the Python
///          mirror all derive from it.
struct NamedPosition {
  DeviceFeatures feature;   ///< The bit a device needs to accept it.
  uint16_t main_parameter;  ///< Value sent as the EXECUTE main parameter.
  CoverCommand command;     ///< The command that sends it.
  const char *name;         ///< cover_command_name() of the command ("FAVORITE", "VENT").
};

/// @brief The named positions.
/// @details Adding one is a bit (with its evidence), a row here, a CoverCommand value, the rows that
///          have it in the type table, and a companion button in the Python codegen.
inline constexpr NamedPosition NAMED_POSITIONS[] = {
    {device_feature::FAVORITE, main_parameter_of(POS_FAVORITE), CoverCommand::FAVORITE, "FAVORITE"},
    {device_feature::VENT, main_parameter_of(POS_FAVORITE, POS_VENT_MODIFIER), CoverCommand::VENT, "VENT"},
};

/// @brief The named position a command sends.
/// @param command Cover command.
/// @return The NAMED_POSITIONS entry, or nullptr for a command that is not a named position.
inline const NamedPosition *find_named_position(CoverCommand command) {
  for (const NamedPosition &named : NAMED_POSITIONS) {
    if (named.command == command)
      return &named;
  }
  return nullptr;
}

/// @brief True when a device with these features accepts this main-parameter value in an EXECUTE.
/// @details Open (0x0000), closed (0xC800) and stop (0xD200) always; 0x0001-0xC7FF only with LEVELS;
///          a named position only with its bit; anything else never.
/// @param features The device's resolved features.
/// @param main_parameter The main parameter an EXECUTE would carry.
constexpr bool device_accepts_main_parameter(DeviceFeatures features, uint16_t main_parameter) {
  if (main_parameter == MAIN_PARAMETER_OPEN || main_parameter == MAIN_PARAMETER_CLOSED ||
      main_parameter == MAIN_PARAMETER_STOP)
    return true;
  if (main_parameter < MAIN_PARAMETER_CLOSED)
    return has_feature(features, device_feature::LEVELS);
  for (const NamedPosition &named : NAMED_POSITIONS) {
    if (named.main_parameter == main_parameter)
      return has_feature(features, named.feature);
  }
  return false;
}

/// @brief The main parameter set_device_position() sends for a percent position or the POS_STOP sentinel.
/// @details The position guard converts with this and never compares a percent against wire values.
/// @param position Percent 0-100 (IO scale), or POS_STOP.
/// @return POS_STOP gives MAIN_PARAMETER_STOP; 0-100 gives the percent on the doubled wire scale;
///         anything else gives MAIN_PARAMETER_INVALID, which no device accepts.
constexpr uint16_t position_main_parameter(uint8_t position) {
  if (position == POS_STOP)
    return MAIN_PARAMETER_STOP;
  if (position > POSITION_WIRE_MAX / POSITION_WIRE_SCALE)
    return MAIN_PARAMETER_INVALID;
  return main_parameter_of(static_cast<uint8_t>(POSITION_WIRE_SCALE * position));
}

}  // namespace home_io_control
}  // namespace esphome
