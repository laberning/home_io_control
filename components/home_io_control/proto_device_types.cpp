/// @file proto_device_types.cpp
/// @brief The IO node-type / subtype table and its lookups.
/// @ingroup hioc_protocol

#include "proto_device_types.h"

#include <cstdio>

namespace esphome {
namespace home_io_control {

namespace {

using device_feature::EXTENDED_POSITION_FRAME;
using device_feature::FAVORITE;
using device_feature::INVERTED;
using device_feature::LEVELS;
using device_feature::TILT;
using device_feature::VENT;
using device_feature::WINDOW_COMPANIONS;
using device_type_flags::HAS_PLATFORM;
using device_type_flags::STANDALONE_NAME;
using device_type_flags::YAML_SELECTABLE;

constexpr DeviceCapabilityClass CAP_NONE = DeviceCapabilityClass::UNKNOWN;
constexpr DeviceCapabilityClass CAP_COVER = DeviceCapabilityClass::COVER;
constexpr DeviceCapabilityClass CAP_LIGHT = DeviceCapabilityClass::LIGHT;
constexpr DeviceCapabilityClass CAP_SWITCH = DeviceCapabilityClass::SWITCH;
constexpr DeviceCapabilityClass CAP_LOCK = DeviceCapabilityClass::LOCK;
constexpr DeviceCapabilityClass CAP_CLIMATE = DeviceCapabilityClass::CLIMATE;
constexpr DeviceCapabilityClass CAP_HEATER_LEVEL = DeviceCapabilityClass::HEATER_LEVEL;

constexpr uint8_t SELECTABLE_PLATFORM =
    YAML_SELECTABLE | HAS_PLATFORM;                 ///< Type row that YAML can name and a platform drives.
constexpr uint8_t SUBTYPE_PLATFORM = HAS_PLATFORM;  ///< Subtype row a platform drives.

// === Feature words ===
//
// A row states its whole word. Families share a constant, and a subtype-58 row is
// main_parameter_only_variant() of its family where it has no reason to differ.

/// Positionable cover: a slider, the favourite position and the 8-byte position frame.
constexpr DeviceFeatures COVER = LEVELS | FAVORITE | EXTENDED_POSITION_FRAME;
/// Cover with an adjustable slat angle.
constexpr DeviceFeatures COVER_TILTING = COVER | TILT;
/// Window opener: a cover with the secured ventilation position.
constexpr DeviceFeatures WINDOW = COVER | VENT;
/// Garage opener: percent positions in the main parameter only (issue #163).
/// \todo The garage opener of issue #163 refuses the favourite alias with PARAMETER_UNKNOWN: remove
///       FAVORITE from this word.
constexpr DeviceFeatures GARAGE = device_feature::MAIN_PARAMETER_ONLY | LEVELS | FAVORITE;
/// Subtype-58 garage opener, gate opener and swinging shutter: main parameter only, though they
/// still offer a slider and the favourite.
/// \todo The 5/58 garage opener of issue #163 refuses percent positions with PARAMETER_UNKNOWN, so
///       these rows should be main_parameter_only_variant() of their family; confirm the same on a
///       real 7/58 gate opener and a real 24/58 swinging shutter (inferred from the shared subtype-58
///       definition).
constexpr DeviceFeatures SLIDER_58_COVER = device_feature::MAIN_PARAMETER_ONLY | LEVELS | FAVORITE;
/// On/off command in the 8-byte frame: a plain light, a switch, a lock.
constexpr DeviceFeatures BINARY_COMMAND = EXTENDED_POSITION_FRAME;
/// Light whose brightness is controllable (the 6/1 and 6/2 rows).
constexpr DeviceFeatures DIMMABLE_LIGHT = LEVELS | EXTENDED_POSITION_FRAME;
/// Exterior heating: a heat level in the main parameter, main parameter only.
constexpr DeviceFeatures HEAT_LEVEL_DEVICE = device_feature::MAIN_PARAMETER_ONLY | LEVELS;

constexpr size_t TYPE_HEX_BUFFER_SIZE = 24;  ///< "unknown type 0x3FF" plus margin.

/// @brief The table, sorted by (type, subtype).
///
/// A row without HAS_PLATFORM is a type the project recognises and names but cannot drive yet:
/// discovery and the roll-call scan report it as "not supported yet" and print no YAML.
/// The capability class is the entity family the device would map to, so a type with a class but no
/// platform (ventilation point, heating interface) is a statement about the device, not a promise.
///
/// Rows that no capture or report has confirmed yet are experimental; the 6/1 and 6/2 light rows are
/// unproven on hardware: \todo Confirm 6/1 and 6/2 accept the brightness EXECUTE on a real device.
/// \todo Confirm the 15/2, 9/1, 9/2, 22/x, 24/58, 27/1 and 1023/2..5 subtype rows against a capture.
constexpr DeviceTypeInfo DEVICE_TYPE_TABLE[] = {
    {DeviceType::UNKNOWN, 0, "unknown", CAP_NONE, YAML_SELECTABLE, 0},

    {DeviceType::INTERIOR_VENETIAN_BLIND, 0, "interior_venetian_blind", CAP_COVER, SELECTABLE_PLATFORM, COVER_TILTING},

    {DeviceType::ROLLER_SHUTTER, 0, "roller_shutter", CAP_COVER, SELECTABLE_PLATFORM, COVER},
    {DeviceType::ROLLER_SHUTTER, 1, "with adjustable slats", CAP_COVER, SUBTYPE_PLATFORM, COVER},
    {DeviceType::ROLLER_SHUTTER, 2, "with projection", CAP_COVER, SUBTYPE_PLATFORM, COVER},

    {DeviceType::VERTICAL_EXTERIOR_AWNING, 0, "vertical_exterior_awning", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::WINDOW_OPENER, 0, "window_opener", CAP_COVER, SELECTABLE_PLATFORM, WINDOW},
    {DeviceType::WINDOW_OPENER, 1, "with integrated rain sensor", CAP_COVER, SUBTYPE_PLATFORM, WINDOW},

    // Every garage opener takes the main parameter only (issue #163); subtype 58 does so on any type.
    {DeviceType::GARAGE_OPENER, 0, "garage_opener", CAP_COVER, SELECTABLE_PLATFORM, GARAGE},
    {DeviceType::GARAGE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY, "position only", CAP_COVER, SUBTYPE_PLATFORM,
     SLIDER_58_COVER},

    {DeviceType::LIGHT, 0, "light", CAP_LIGHT, SELECTABLE_PLATFORM, BINARY_COMMAND},
    {DeviceType::LIGHT, 1, "dimmable RGB", CAP_LIGHT, SUBTYPE_PLATFORM, DIMMABLE_LIGHT},
    {DeviceType::LIGHT, 2, "tunable white", CAP_LIGHT, SUBTYPE_PLATFORM, DIMMABLE_LIGHT},
    {DeviceType::LIGHT, SUBTYPE_MAIN_PARAMETER_ONLY, "on/off only", CAP_LIGHT, SUBTYPE_PLATFORM,
     main_parameter_only_variant(BINARY_COMMAND)},

    {DeviceType::GATE_OPENER, 0, "gate_opener", CAP_COVER, SELECTABLE_PLATFORM, COVER},
    {DeviceType::GATE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY, "position only", CAP_COVER, SUBTYPE_PLATFORM,
     SLIDER_58_COVER},

    {DeviceType::ROLLING_DOOR_OPENER, 0, "rolling_door_opener", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::LOCK, 0, "lock", CAP_LOCK, SELECTABLE_PLATFORM, BINARY_COMMAND},
    {DeviceType::LOCK, 1, "window lock", CAP_LOCK, SUBTYPE_PLATFORM | STANDALONE_NAME, BINARY_COMMAND},
    {DeviceType::LOCK, 2, "car controller", CAP_NONE, STANDALONE_NAME, 0},

    {DeviceType::VERTICAL_INTERIOR_BLIND, 0, "vertical_interior_blind", CAP_COVER, SELECTABLE_PLATFORM, COVER_TILTING},

    {DeviceType::SECURE_CONFIGURATION_DEVICE, 0, "secure_configuration_device", CAP_NONE, 0, 0},

    {DeviceType::BEACON, 0, "beacon", DeviceCapabilityClass::BEACON, 0, 0},

    {DeviceType::DUAL_ROLLER_SHUTTER, 0, "dual_roller_shutter", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, "heating_temperature_interface", CAP_CLIMATE, SELECTABLE_PLATFORM,
     0},

    {DeviceType::ON_OFF_SWITCH, 0, "on_off_switch", CAP_SWITCH, SELECTABLE_PLATFORM, BINARY_COMMAND},
    {DeviceType::ON_OFF_SWITCH, 2, "micromodule", CAP_SWITCH, SUBTYPE_PLATFORM, BINARY_COMMAND},

    {DeviceType::HORIZONTAL_AWNING, 0, "horizontal_awning", CAP_COVER, SELECTABLE_PLATFORM, COVER | INVERTED},
    {DeviceType::HORIZONTAL_AWNING, 1, "rail-guided pergola", CAP_COVER, SUBTYPE_PLATFORM, COVER | INVERTED},

    {DeviceType::EXTERIOR_VENETIAN_BLIND, 0, "exterior_venetian_blind", CAP_COVER, SELECTABLE_PLATFORM, COVER_TILTING},

    {DeviceType::LOUVRE_BLIND, 0, "louvre_blind", CAP_COVER, SELECTABLE_PLATFORM, COVER_TILTING},

    {DeviceType::CURTAIN_TRACK, 0, "curtain_track", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::VENTILATION_POINT, 0, "ventilation_point", CAP_SWITCH, 0, 0},
    {DeviceType::VENTILATION_POINT, 1, "air inlet", CAP_SWITCH, 0, 0},
    {DeviceType::VENTILATION_POINT, 2, "air transfer", CAP_SWITCH, 0, 0},
    {DeviceType::VENTILATION_POINT, 3, "air outlet", CAP_SWITCH, 0, 0},

    // Exterior heating takes a heat level in the position field (a fan entity), not a setpoint. Every
    // exterior heater gets the main parameter only: its first functional parameter is not a cover's, and
    // a guessed block is what issue #163 showed to cost a refusal.
    {DeviceType::EXTERIOR_HEATING, 0, "exterior_heating", CAP_HEATER_LEVEL, SELECTABLE_PLATFORM, HEAT_LEVEL_DEVICE},
    {DeviceType::EXTERIOR_HEATING, SUBTYPE_MAIN_PARAMETER_ONLY, "on/off only", CAP_HEATER_LEVEL, SUBTYPE_PLATFORM,
     main_parameter_only_variant(HEAT_LEVEL_DEVICE)},

    {DeviceType::HEAT_PUMP, 0, "heat_pump", CAP_CLIMATE, SELECTABLE_PLATFORM, 0},
    // The two Atlantic profiles use a different private register family from the electrical
    // heater's, so the heating payloads must not be sent to them: no climate class.
    {DeviceType::HEAT_PUMP, 1, "heat pump (Atlantic)", CAP_NONE, STANDALONE_NAME, 0},
    {DeviceType::HEAT_PUMP, 2, "boiler (Atlantic)", CAP_NONE, STANDALONE_NAME, 0},

    {DeviceType::INTRUSION_ALARM, 0, "intrusion_alarm", DeviceCapabilityClass::SENSOR, YAML_SELECTABLE, 0},

    {DeviceType::SWINGING_SHUTTER, 0, "swinging_shutter", CAP_COVER, SELECTABLE_PLATFORM, COVER},
    {DeviceType::SWINGING_SHUTTER, 1, "with independent leaves", CAP_COVER, SUBTYPE_PLATFORM, COVER},
    {DeviceType::SWINGING_SHUTTER, SUBTYPE_MAIN_PARAMETER_ONLY, "position only", CAP_COVER, SUBTYPE_PLATFORM,
     SLIDER_58_COVER},

    {DeviceType::SLIDING_WINDOW, 0, "sliding_window", CAP_COVER, SELECTABLE_PLATFORM, COVER},
    {DeviceType::SLIDING_WINDOW, 1, "with lock", CAP_COVER, SUBTYPE_PLATFORM, COVER},

    {DeviceType::ZONE_CONTROL, 0, "zone_control", CAP_NONE, 0, 0},

    {DeviceType::BIOCLIMATIC_PERGOLA, 0, "bioclimatic_pergola", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::SIREN, 0, "siren", CAP_NONE, 0, 0},

    {DeviceType::DOMESTIC_HOT_WATER, 0, "domestic_hot_water", CAP_NONE, 0, 0},

    // Both rows have a heating profile (proto_heating_profiles.cpp): the plain heater is commanded with
    // heating levels in the standard EXECUTE, the one with an adjustable setpoint over the private
    // registers (proto_heating.h).
    {DeviceType::ELECTRICAL_HEATER, 0, "electrical_heater", CAP_CLIMATE, SELECTABLE_PLATFORM, 0},
    {DeviceType::ELECTRICAL_HEATER, 1, "with adjustable setpoint", CAP_CLIMATE, SUBTYPE_PLATFORM, WINDOW_COMPANIONS},

    {DeviceType::HEAT_RECOVERY_VENTILATION, 0, "heat_recovery_ventilation", CAP_NONE, 0, 0},

    {DeviceType::CENTRAL_HOUSE_CONTROL, 0, "central_house_control", CAP_NONE, 0, 0},

    {DeviceType::TEST_AND_EVALUATION, 0, "test_and_evaluation", CAP_NONE, 0, 0},

    {DeviceType::REMOTE_CONTROLLER, 0, "remote_controller", CAP_NONE, 0, 0},
    {DeviceType::REMOTE_CONTROLLER, 2, "Keygo", CAP_NONE, 0, 0},
    {DeviceType::REMOTE_CONTROLLER, 3, "Izymo", CAP_NONE, 0, 0},
    {DeviceType::REMOTE_CONTROLLER, 4, "public frame", CAP_NONE, 0, 0},
    {DeviceType::REMOTE_CONTROLLER, 5, "V500", CAP_NONE, 0, 0},
};

}  // namespace

const DeviceTypeInfo *find_exact_device_type_info(DeviceType type, uint8_t subtype) {
  for (const DeviceTypeInfo &row : DEVICE_TYPE_TABLE) {
    if (row.type == type && row.subtype == subtype)
      return &row;
  }
  return nullptr;
}

const DeviceTypeInfo *find_device_type_info(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *row = find_exact_device_type_info(type, subtype);
  return row != nullptr || subtype == 0 ? row : find_exact_device_type_info(type, 0);
}

bool device_has_platform(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *row = find_device_type_info(type, subtype);
  return row != nullptr && (row->flags & HAS_PLATFORM) != 0;
}

const char *device_platform_name(DeviceType type, uint8_t subtype) {
  if (!device_has_platform(type, subtype))
    return nullptr;
  switch (find_device_type_info(type, subtype)->capability) {
    case DeviceCapabilityClass::COVER:
      return "cover";
    case DeviceCapabilityClass::LIGHT:
      return "light";
    case DeviceCapabilityClass::SWITCH:
      return "switch";
    case DeviceCapabilityClass::LOCK:
      return "lock";
    case DeviceCapabilityClass::CLIMATE:
      return "climate";
    case DeviceCapabilityClass::HEATER_LEVEL:
      return "fan";
    // No platform drives these classes; listed so -Wswitch flags a new class that needs a decision.
    case DeviceCapabilityClass::UNKNOWN:
    case DeviceCapabilityClass::SENSOR:
    case DeviceCapabilityClass::BEACON:
      return nullptr;
  }
  return nullptr;
}

DeviceCapabilityClass device_capability_class(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *row = find_device_type_info(type, subtype);
  return row != nullptr ? row->capability : DeviceCapabilityClass::UNKNOWN;
}

DeviceFeatures device_features(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *exact = find_exact_device_type_info(type, subtype);
  if (exact != nullptr)
    return exact->features;
  const DeviceTypeInfo *type_row = find_exact_device_type_info(type, 0);
  if (subtype == SUBTYPE_MAIN_PARAMETER_ONLY)
    return main_parameter_only_variant(type_row != nullptr ? type_row->features : DeviceFeatures{0});
  return type_row != nullptr ? type_row->features : DeviceFeatures{0};
}

DeviceFeatures undeclared_kind_features(DeviceCapabilityClass entity_class) {
  switch (entity_class) {
    case DeviceCapabilityClass::COVER:
      return COVER | VENT;
    case DeviceCapabilityClass::LIGHT:
    case DeviceCapabilityClass::SWITCH:
    case DeviceCapabilityClass::LOCK:
      return BINARY_COMMAND;
    case DeviceCapabilityClass::HEATER_LEVEL:
      return LEVELS | EXTENDED_POSITION_FRAME;
    // No EXECUTE position path; listed so -Wswitch flags a new class that needs a decision.
    case DeviceCapabilityClass::UNKNOWN:
    case DeviceCapabilityClass::SENSOR:
    case DeviceCapabilityClass::BEACON:
    case DeviceCapabilityClass::CLIMATE:
      return 0;
  }
  return 0;
}

std::string format_device_kind(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *type_row = find_exact_device_type_info(type, 0);
  if (type_row == nullptr) {
    char buf[TYPE_HEX_BUFFER_SIZE];
    snprintf(buf, sizeof(buf), "unknown type 0x%02X", static_cast<unsigned>(type));
    return subtype == 0 ? std::string(buf) : std::string(buf) + ", subtype " + std::to_string(subtype);
  }

  const DeviceTypeInfo *subtype_row = subtype == 0 ? nullptr : find_exact_device_type_info(type, subtype);
  if (subtype_row != nullptr && (subtype_row->flags & STANDALONE_NAME) != 0)
    return subtype_row->name;

  // Identifiers are lowercase words joined by underscores; the display form swaps in spaces.
  std::string kind = type_row->name;
  for (char &c : kind) {
    if (c == '_')
      c = ' ';
  }
  if (subtype == 0)
    return kind;
  return kind + ", " + (subtype_row != nullptr ? std::string(subtype_row->name) : "subtype " + std::to_string(subtype));
}

std::string format_device_ids(DeviceType type, uint8_t subtype) {
  char buf[TYPE_HEX_BUFFER_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X/%u", static_cast<unsigned>(type), static_cast<unsigned>(subtype));
  return buf;
}

}  // namespace home_io_control
}  // namespace esphome
