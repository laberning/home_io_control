/// @file proto_device_types.cpp
/// @brief The IO node-type / subtype table and its lookups.
/// @ingroup hioc_protocol

#include "proto_device_types.h"

#include <cstdio>

namespace esphome {
namespace home_io_control {

namespace {

using device_type_flags::BRIGHTNESS_ONLY;
using device_type_flags::HAS_PLATFORM;
using device_type_flags::MAIN_PARAMETER_ONLY;
using device_type_flags::STANDALONE_NAME;
using device_type_flags::YAML_SELECTABLE;

constexpr DeviceCapabilityClass CAP_NONE = DeviceCapabilityClass::UNKNOWN;
constexpr DeviceCapabilityClass CAP_COVER = DeviceCapabilityClass::COVER;
constexpr DeviceCapabilityClass CAP_LIGHT = DeviceCapabilityClass::LIGHT;
constexpr DeviceCapabilityClass CAP_SWITCH = DeviceCapabilityClass::SWITCH;
constexpr DeviceCapabilityClass CAP_LOCK = DeviceCapabilityClass::LOCK;
constexpr DeviceCapabilityClass CAP_CLIMATE = DeviceCapabilityClass::CLIMATE;

constexpr uint8_t SELECTABLE_PLATFORM =
    YAML_SELECTABLE | HAS_PLATFORM;                 ///< Type row that YAML can name and a platform drives.
constexpr uint8_t SUBTYPE_PLATFORM = HAS_PLATFORM;  ///< Subtype row a platform drives.

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
    {DeviceType::UNKNOWN, 0, "unknown", CAP_NONE, YAML_SELECTABLE},

    {DeviceType::INTERIOR_VENETIAN_BLIND, 0, "interior_venetian_blind", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::ROLLER_SHUTTER, 0, "roller_shutter", CAP_COVER, SELECTABLE_PLATFORM},
    {DeviceType::ROLLER_SHUTTER, 1, "with adjustable slats", CAP_COVER, SUBTYPE_PLATFORM},
    {DeviceType::ROLLER_SHUTTER, 2, "with projection", CAP_COVER, SUBTYPE_PLATFORM},

    {DeviceType::VERTICAL_EXTERIOR_AWNING, 0, "vertical_exterior_awning", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::WINDOW_OPENER, 0, "window_opener", CAP_COVER, SELECTABLE_PLATFORM},
    {DeviceType::WINDOW_OPENER, 1, "with integrated rain sensor", CAP_COVER, SUBTYPE_PLATFORM},

    // Every garage opener takes the main parameter only (issue #163); subtype 58 does so on any type.
    {DeviceType::GARAGE_OPENER, 0, "garage_opener", CAP_COVER, SELECTABLE_PLATFORM | MAIN_PARAMETER_ONLY},
    {DeviceType::GARAGE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY, "position only", CAP_COVER, SUBTYPE_PLATFORM},

    {DeviceType::LIGHT, 0, "light", CAP_LIGHT, SELECTABLE_PLATFORM},
    {DeviceType::LIGHT, 1, "dimmable RGB", CAP_LIGHT, SUBTYPE_PLATFORM | BRIGHTNESS_ONLY},
    {DeviceType::LIGHT, 2, "tunable white", CAP_LIGHT, SUBTYPE_PLATFORM | BRIGHTNESS_ONLY},
    {DeviceType::LIGHT, SUBTYPE_MAIN_PARAMETER_ONLY, "on/off only", CAP_LIGHT, SUBTYPE_PLATFORM},

    {DeviceType::GATE_OPENER, 0, "gate_opener", CAP_COVER, SELECTABLE_PLATFORM},
    {DeviceType::GATE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY, "position only", CAP_COVER, SUBTYPE_PLATFORM},

    {DeviceType::ROLLING_DOOR_OPENER, 0, "rolling_door_opener", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::LOCK, 0, "lock", CAP_LOCK, SELECTABLE_PLATFORM},
    {DeviceType::LOCK, 1, "window lock", CAP_LOCK, SUBTYPE_PLATFORM | STANDALONE_NAME},
    {DeviceType::LOCK, 2, "car controller", CAP_NONE, STANDALONE_NAME},

    {DeviceType::VERTICAL_INTERIOR_BLIND, 0, "vertical_interior_blind", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::SECURE_CONFIGURATION_DEVICE, 0, "secure_configuration_device", CAP_NONE, 0},

    {DeviceType::BEACON, 0, "beacon", DeviceCapabilityClass::BEACON, 0},

    {DeviceType::DUAL_ROLLER_SHUTTER, 0, "dual_roller_shutter", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, "heating_temperature_interface", CAP_CLIMATE, 0},

    {DeviceType::ON_OFF_SWITCH, 0, "on_off_switch", CAP_SWITCH, SELECTABLE_PLATFORM},
    {DeviceType::ON_OFF_SWITCH, 2, "micromodule", CAP_SWITCH, SUBTYPE_PLATFORM},

    {DeviceType::HORIZONTAL_AWNING, 0, "horizontal_awning", CAP_COVER, SELECTABLE_PLATFORM},
    {DeviceType::HORIZONTAL_AWNING, 1, "rail-guided pergola", CAP_COVER, SUBTYPE_PLATFORM},

    {DeviceType::EXTERIOR_VENETIAN_BLIND, 0, "exterior_venetian_blind", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::LOUVRE_BLIND, 0, "louvre_blind", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::CURTAIN_TRACK, 0, "curtain_track", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::VENTILATION_POINT, 0, "ventilation_point", CAP_SWITCH, 0},
    {DeviceType::VENTILATION_POINT, 1, "air inlet", CAP_SWITCH, 0},
    {DeviceType::VENTILATION_POINT, 2, "air transfer", CAP_SWITCH, 0},
    {DeviceType::VENTILATION_POINT, 3, "air outlet", CAP_SWITCH, 0},

    {DeviceType::EXTERIOR_HEATING, 0, "exterior_heating", CAP_CLIMATE, 0},
    {DeviceType::EXTERIOR_HEATING, SUBTYPE_MAIN_PARAMETER_ONLY, "on/off only", CAP_CLIMATE, 0},

    {DeviceType::HEAT_PUMP, 0, "heat_pump", CAP_CLIMATE, 0},
    // The two Atlantic profiles use a different private register family from the electrical
    // heater's, so the heating payloads must not be sent to them: no climate class.
    {DeviceType::HEAT_PUMP, 1, "heat pump (Atlantic)", CAP_NONE, STANDALONE_NAME},
    {DeviceType::HEAT_PUMP, 2, "boiler (Atlantic)", CAP_NONE, STANDALONE_NAME},

    {DeviceType::INTRUSION_ALARM, 0, "intrusion_alarm", DeviceCapabilityClass::SENSOR, YAML_SELECTABLE},

    {DeviceType::SWINGING_SHUTTER, 0, "swinging_shutter", CAP_COVER, SELECTABLE_PLATFORM},
    {DeviceType::SWINGING_SHUTTER, 1, "with independent leaves", CAP_COVER, SUBTYPE_PLATFORM},
    {DeviceType::SWINGING_SHUTTER, SUBTYPE_MAIN_PARAMETER_ONLY, "position only", CAP_COVER, SUBTYPE_PLATFORM},

    {DeviceType::SLIDING_WINDOW, 0, "sliding_window", CAP_COVER, SELECTABLE_PLATFORM},
    {DeviceType::SLIDING_WINDOW, 1, "with lock", CAP_COVER, SUBTYPE_PLATFORM},

    {DeviceType::ZONE_CONTROL, 0, "zone_control", CAP_NONE, 0},

    {DeviceType::BIOCLIMATIC_PERGOLA, 0, "bioclimatic_pergola", CAP_COVER, SELECTABLE_PLATFORM},

    {DeviceType::SIREN, 0, "siren", CAP_NONE, 0},

    {DeviceType::DOMESTIC_HOT_WATER, 0, "domestic_hot_water", CAP_NONE, 0},

    // Both rows have a heating profile (proto_heating_profiles.cpp): the plain heater is commanded with
    // heating levels in the standard EXECUTE, the one with an adjustable setpoint over the private
    // registers (proto_heating.h).
    {DeviceType::ELECTRICAL_HEATER, 0, "electrical_heater", CAP_CLIMATE, SELECTABLE_PLATFORM},
    {DeviceType::ELECTRICAL_HEATER, 1, "with adjustable setpoint", CAP_CLIMATE, SUBTYPE_PLATFORM},

    {DeviceType::HEAT_RECOVERY_VENTILATION, 0, "heat_recovery_ventilation", CAP_NONE, 0},

    {DeviceType::CENTRAL_HOUSE_CONTROL, 0, "central_house_control", CAP_NONE, 0},

    {DeviceType::TEST_AND_EVALUATION, 0, "test_and_evaluation", CAP_NONE, 0},

    {DeviceType::REMOTE_CONTROLLER, 0, "remote_controller", CAP_NONE, 0},
    {DeviceType::REMOTE_CONTROLLER, 2, "Keygo", CAP_NONE, 0},
    {DeviceType::REMOTE_CONTROLLER, 3, "Izymo", CAP_NONE, 0},
    {DeviceType::REMOTE_CONTROLLER, 4, "public frame", CAP_NONE, 0},
    {DeviceType::REMOTE_CONTROLLER, 5, "V500", CAP_NONE, 0},
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

bool device_light_is_brightness_only(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *row = find_device_type_info(type, subtype);
  return row != nullptr && (row->flags & BRIGHTNESS_ONLY) != 0;
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
