/// @file proto_device_types.cpp
/// @brief The IO node-type / subtype table and its lookups.
/// @ingroup hioc_protocol

#include "proto_device_types.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace esphome {
namespace home_io_control {

namespace {

using device_feature::EXTENDED_POSITION_FRAME;
using device_feature::FAVORITE;
using device_feature::INVERTED;
using device_feature::LEVELS;
using device_feature::PARTIAL;
using device_feature::PEDESTRIAN;
using device_feature::TILT;
using device_feature::VENT;
using device_feature::WINDOW_COMPANIONS;
using device_type_flags::EXPERIMENTAL;
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
constexpr DeviceCapabilityClass CAP_WATER_HEATER = DeviceCapabilityClass::WATER_HEATER;

constexpr uint8_t SELECTABLE_PLATFORM =
    YAML_SELECTABLE | HAS_PLATFORM;                 ///< Type row that YAML can name and a platform drives.
constexpr uint8_t SUBTYPE_PLATFORM = HAS_PLATFORM;  ///< Subtype row a platform drives.
constexpr uint8_t EXPERIMENTAL_SUBTYPE_PLATFORM =
    HAS_PLATFORM | EXPERIMENTAL;  ///< Subtype row a platform drives that no capture or report confirms.

// === Feature words ===
//
// A row states its whole word. Families share a constant, and a subtype-58 row is
// main_parameter_only_variant() of its family where it has no reason to differ.

/// Positionable cover: a slider, the favourite position and the 8-byte position frame.
constexpr DeviceFeatures COVER = LEVELS | FAVORITE | EXTENDED_POSITION_FRAME;
/// Cover with an adjustable slat angle.
constexpr DeviceFeatures COVER_TILTING = COVER | TILT;
/// Interior venetian blind (type 1): functional parameter 1 is the slat orientation, not a travel
/// speed (VELUX KLF 200 API specification, Appendix 2), so positions go out in the 6-byte form
/// without EXTENDED_POSITION_FRAME; tilt stays on functional parameter 3.
/// \todo Confirm on a real two-way interior venetian blind that the 6-byte position frame moves it and
///       that the tilt frame on functional parameter 3 still orients the slats (experimental).
constexpr DeviceFeatures INTERIOR_VENETIAN = LEVELS | FAVORITE | TILT;
/// Dual roller shutter (type 13): functional parameter 1 is the upper curtain and the speed is
/// parameter 4 (VELUX KLF 200 API specification, Appendix 2), so positions go out in the 6-byte form.
/// \todo Confirm on a real two-way dual roller shutter that the 6-byte position frame moves both
///       curtains and that a position needs no functional parameter (experimental).
constexpr DeviceFeatures DUAL_SHUTTER = LEVELS | FAVORITE;
/// Vertical interior blind (type 10): the KLF 200 parameter table lists no slat parameter for it
/// (VELUX KLF 200 API specification, Appendix 2), so it is a plain cover.
/// \todo Confirm on a real two-way vertical interior blind that it has no slat angle (experimental).
constexpr DeviceFeatures VERTICAL_INTERIOR = COVER;
/// Window opener: a cover with the secured ventilation position.
constexpr DeviceFeatures WINDOW = COVER | VENT;
/// Garage opener: percent positions in the main parameter only, and no favourite position. The
/// garage opener of issue #163 refuses the favourite alias with PARAMETER_UNKNOWN (corpus capture
/// wisniowski_moto1000_exchange_execute_parameter_unknown); that rests on one device of this type,
/// so it is applied to the whole family.
/// It also carries PARTIAL, the Partial Position button (0xD809), which runs the opener's stored
/// partial position (confirmed on the issue #163 opener).
constexpr DeviceFeatures GARAGE = device_feature::MAIN_PARAMETER_ONLY | LEVELS | PARTIAL;
/// Gate opener: a positionable cover that also carries PEDESTRIAN, the Pedestrian Position button
/// (0xD807).
/// \todo Confirm on a real gate opener that 0xD807 opens it to a pedestrian position (experimental).
constexpr DeviceFeatures GATE = COVER | PEDESTRIAN;
/// On/off command in the 8-byte frame: a plain light, a switch, a lock.
constexpr DeviceFeatures BINARY_COMMAND = EXTENDED_POSITION_FRAME;
/// Light whose brightness is controllable (the 6/1 and 6/2 rows).
constexpr DeviceFeatures DIMMABLE_LIGHT = LEVELS | EXTENDED_POSITION_FRAME;
/// Exterior heating: a heat level in the main parameter, main parameter only.
constexpr DeviceFeatures HEAT_LEVEL_DEVICE = device_feature::MAIN_PARAMETER_ONLY | LEVELS;

/// Model of every type and subtype row: the row is the (type, subtype) itself.
constexpr DeviceModel NO_MODEL = DeviceModel::NONE;

constexpr size_t TYPE_HEX_BUFFER_SIZE = 24;  ///< "unknown type 0x3FF" plus margin.

/// @brief The table, sorted by (type, subtype); a model row follows the (type, subtype) row it belongs to.
///
/// A row without HAS_PLATFORM is a type the project recognises and names but cannot drive yet:
/// discovery and the roll-call scan report it as "not supported yet" and print no YAML.
/// The capability class is the entity family the device would map to, so a type with a class but no
/// platform (ventilation point, heating interface) is a statement about the device, not a promise.
///
/// Rows that no capture or report has confirmed yet carry device_type_flags::EXPERIMENTAL: the 6/1,
/// 6/2, 7/58, 9/1, 9/2, 15/2, 22/1, 22/2, 24/58, 27/1 and 1023/2..5 subtype rows. Discovery and pairing
/// log them as experimental and the codegen warns when a config declares one. The flag leaves with the
/// \todo that names a row once a capture or a reporter confirms it. Rows 1, 10 and 13 are not flagged:
/// their changed behaviour is experimental, their identity is not. The 51/0 type row is the one type row
/// that carries the flag: one heater of one brand has been read, its writes are unconfirmed, and the flag
/// is what makes the codegen warn a user of any other water heater. The flag names its row only: a
/// subtype without a row of its own does not inherit it (device_is_experimental(), and the Python mirror).
/// \todo Confirm 6/1 and 6/2 accept the brightness EXECUTE on a real device.
/// \todo Confirm the 15/2, 9/1, 9/2, 22/x, 7/58, 24/58, 27/1 and 1023/2..5 subtype rows against a capture.
constexpr DeviceTypeInfo DEVICE_TYPE_TABLE[] = {
    {DeviceType::UNKNOWN, 0, NO_MODEL, "unknown", CAP_NONE, YAML_SELECTABLE, 0},

    {DeviceType::INTERIOR_VENETIAN_BLIND, 0, NO_MODEL, "interior_venetian_blind", CAP_COVER, SELECTABLE_PLATFORM,
     INTERIOR_VENETIAN},

    {DeviceType::ROLLER_SHUTTER, 0, NO_MODEL, "roller_shutter", CAP_COVER, SELECTABLE_PLATFORM, COVER},
    {DeviceType::ROLLER_SHUTTER, 1, NO_MODEL, "with adjustable slats", CAP_COVER, SUBTYPE_PLATFORM, COVER},
    {DeviceType::ROLLER_SHUTTER, 2, NO_MODEL, "with projection", CAP_COVER, SUBTYPE_PLATFORM, COVER},

    {DeviceType::VERTICAL_EXTERIOR_AWNING, 0, NO_MODEL, "vertical_exterior_awning", CAP_COVER, SELECTABLE_PLATFORM,
     COVER},

    {DeviceType::WINDOW_OPENER, 0, NO_MODEL, "window_opener", CAP_COVER, SELECTABLE_PLATFORM, WINDOW},
    {DeviceType::WINDOW_OPENER, 1, NO_MODEL, "with integrated rain sensor", CAP_COVER, SUBTYPE_PLATFORM, WINDOW},

    // Every garage opener takes the main parameter only (issue #163); subtype 58 does so on any type.
    {DeviceType::GARAGE_OPENER, 0, NO_MODEL, "garage_opener", CAP_COVER, SELECTABLE_PLATFORM, GARAGE},
    // Subtype 58 offers open, close and stop only: the 5/58 garage opener of issue #163 refuses a 50 %
    // position with PARAMETER_UNKNOWN (corpus capture wisniowski_moto1000_exchange_execute_partial_position_refused).
    {DeviceType::GARAGE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY, NO_MODEL, "open/close only", CAP_COVER, SUBTYPE_PLATFORM,
     main_parameter_only_variant(GARAGE)},

    {DeviceType::LIGHT, 0, NO_MODEL, "light", CAP_LIGHT, SELECTABLE_PLATFORM, BINARY_COMMAND},
    {DeviceType::LIGHT, 1, NO_MODEL, "dimmable RGB", CAP_LIGHT, EXPERIMENTAL_SUBTYPE_PLATFORM, DIMMABLE_LIGHT},
    {DeviceType::LIGHT, 2, NO_MODEL, "tunable white", CAP_LIGHT, EXPERIMENTAL_SUBTYPE_PLATFORM, DIMMABLE_LIGHT},
    {DeviceType::LIGHT, SUBTYPE_MAIN_PARAMETER_ONLY, NO_MODEL, "on/off only", CAP_LIGHT, SUBTYPE_PLATFORM,
     main_parameter_only_variant(BINARY_COMMAND)},

    {DeviceType::GATE_OPENER, 0, NO_MODEL, "gate_opener", CAP_COVER, SELECTABLE_PLATFORM, GATE},
    // \todo Confirm on a real 7/58 gate opener that intermediate positions are refused and whether the
    //       favourite is accepted; inferred from the shared subtype-58 definition and the 5/58 opener of
    //       issue #163 (experimental).
    {DeviceType::GATE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY, NO_MODEL, "open/close only", CAP_COVER,
     EXPERIMENTAL_SUBTYPE_PLATFORM, main_parameter_only_variant(GATE)},

    {DeviceType::ROLLING_DOOR_OPENER, 0, NO_MODEL, "rolling_door_opener", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::LOCK, 0, NO_MODEL, "lock", CAP_LOCK, SELECTABLE_PLATFORM, BINARY_COMMAND},
    {DeviceType::LOCK, 1, NO_MODEL, "window lock", CAP_LOCK, EXPERIMENTAL_SUBTYPE_PLATFORM | STANDALONE_NAME,
     BINARY_COMMAND},
    {DeviceType::LOCK, 2, NO_MODEL, "car controller", CAP_NONE, EXPERIMENTAL | STANDALONE_NAME, 0},

    {DeviceType::VERTICAL_INTERIOR_BLIND, 0, NO_MODEL, "vertical_interior_blind", CAP_COVER, SELECTABLE_PLATFORM,
     VERTICAL_INTERIOR},

    {DeviceType::SECURE_CONFIGURATION_DEVICE, 0, NO_MODEL, "secure_configuration_device", CAP_NONE, 0, 0},

    {DeviceType::BEACON, 0, NO_MODEL, "beacon", DeviceCapabilityClass::BEACON, 0, 0},

    {DeviceType::DUAL_ROLLER_SHUTTER, 0, NO_MODEL, "dual_roller_shutter", CAP_COVER, SELECTABLE_PLATFORM, DUAL_SHUTTER},

    {DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, NO_MODEL, "heating_temperature_interface", CAP_CLIMATE,
     SELECTABLE_PLATFORM, 0},

    {DeviceType::ON_OFF_SWITCH, 0, NO_MODEL, "on_off_switch", CAP_SWITCH, SELECTABLE_PLATFORM, BINARY_COMMAND},
    {DeviceType::ON_OFF_SWITCH, 2, NO_MODEL, "micromodule", CAP_SWITCH, EXPERIMENTAL_SUBTYPE_PLATFORM, BINARY_COMMAND},

    {DeviceType::HORIZONTAL_AWNING, 0, NO_MODEL, "horizontal_awning", CAP_COVER, SELECTABLE_PLATFORM, COVER | INVERTED},
    {DeviceType::HORIZONTAL_AWNING, 1, NO_MODEL, "rail-guided pergola", CAP_COVER, SUBTYPE_PLATFORM, COVER | INVERTED},

    {DeviceType::EXTERIOR_VENETIAN_BLIND, 0, NO_MODEL, "exterior_venetian_blind", CAP_COVER, SELECTABLE_PLATFORM,
     COVER_TILTING},

    {DeviceType::LOUVRE_BLIND, 0, NO_MODEL, "louvre_blind", CAP_COVER, SELECTABLE_PLATFORM, COVER_TILTING},

    {DeviceType::CURTAIN_TRACK, 0, NO_MODEL, "curtain_track", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::VENTILATION_POINT, 0, NO_MODEL, "ventilation_point", CAP_SWITCH, 0, 0},
    {DeviceType::VENTILATION_POINT, 1, NO_MODEL, "air inlet", CAP_SWITCH, 0, 0},
    {DeviceType::VENTILATION_POINT, 2, NO_MODEL, "air transfer", CAP_SWITCH, 0, 0},
    {DeviceType::VENTILATION_POINT, 3, NO_MODEL, "air outlet", CAP_SWITCH, 0, 0},

    // Exterior heating takes a heat level in the position field (a fan entity), not a setpoint. Every
    // exterior heater gets the main parameter only: its first functional parameter is not a cover's, and
    // a guessed block is what issue #163 showed to cost a refusal.
    {DeviceType::EXTERIOR_HEATING, 0, NO_MODEL, "exterior_heating", CAP_HEATER_LEVEL, SELECTABLE_PLATFORM,
     HEAT_LEVEL_DEVICE},
    {DeviceType::EXTERIOR_HEATING, SUBTYPE_MAIN_PARAMETER_ONLY, NO_MODEL, "on/off only", CAP_HEATER_LEVEL,
     SUBTYPE_PLATFORM, main_parameter_only_variant(HEAT_LEVEL_DEVICE)},

    {DeviceType::HEAT_PUMP, 0, NO_MODEL, "heat_pump", CAP_CLIMATE, SELECTABLE_PLATFORM, 0},
    // The two Atlantic profiles use a different private register family from the electrical
    // heater's, so the heating payloads must not be sent to them: no climate class.
    {DeviceType::HEAT_PUMP, 1, NO_MODEL, "heat pump (Atlantic)", CAP_NONE, EXPERIMENTAL | STANDALONE_NAME, 0},
    {DeviceType::HEAT_PUMP, 2, NO_MODEL, "boiler (Atlantic)", CAP_NONE, EXPERIMENTAL | STANDALONE_NAME, 0},

    {DeviceType::INTRUSION_ALARM, 0, NO_MODEL, "intrusion_alarm", DeviceCapabilityClass::SENSOR, YAML_SELECTABLE, 0},

    {DeviceType::SWINGING_SHUTTER, 0, NO_MODEL, "swinging_shutter", CAP_COVER, SELECTABLE_PLATFORM, COVER},
    {DeviceType::SWINGING_SHUTTER, 1, NO_MODEL, "with independent leaves", CAP_COVER, SUBTYPE_PLATFORM, COVER},
    // \todo Confirm on a real 24/58 swinging shutter that the favourite is accepted and percent positions
    //       are refused; inferred from the shared subtype-58 definition and the 5/58 opener of issue #163
    //       (experimental).
    {DeviceType::SWINGING_SHUTTER, SUBTYPE_MAIN_PARAMETER_ONLY, NO_MODEL, "open/close only", CAP_COVER,
     EXPERIMENTAL_SUBTYPE_PLATFORM, main_parameter_only_variant(COVER)},

    {DeviceType::SLIDING_WINDOW, 0, NO_MODEL, "sliding_window", CAP_COVER, SELECTABLE_PLATFORM, COVER},
    {DeviceType::SLIDING_WINDOW, 1, NO_MODEL, "with lock", CAP_COVER, EXPERIMENTAL_SUBTYPE_PLATFORM, COVER},

    {DeviceType::ZONE_CONTROL, 0, NO_MODEL, "zone_control", CAP_NONE, 0, 0},

    {DeviceType::BIOCLIMATIC_PERGOLA, 0, NO_MODEL, "bioclimatic_pergola", CAP_COVER, SELECTABLE_PLATFORM, COVER},

    {DeviceType::SIREN, 0, NO_MODEL, "siren", CAP_NONE, 0, 0},

    // A domestic hot water heater: readings and mode in family-0x00 registers, the target in the status
    // main parameter (proto_dhw.h). Read on one Atlantic Calypso Connecte (issue #198); its writes are
    // unconfirmed.
    // \todo Confirm on a real heater that the target EXECUTE and the mode write take effect, then drop
    //       EXPERIMENTAL.
    {DeviceType::DOMESTIC_HOT_WATER, 0, NO_MODEL, "domestic_hot_water", CAP_WATER_HEATER,
     SELECTABLE_PLATFORM | EXPERIMENTAL, device_feature::WATER_HEATER},

    // Both rows have a heating profile (proto_heating_profiles.cpp): the plain heater is commanded with
    // heating levels in the standard EXECUTE, the one with an adjustable setpoint over the private
    // registers (proto_heating.h).
    {DeviceType::ELECTRICAL_HEATER, 0, NO_MODEL, "electrical_heater", CAP_CLIMATE, SELECTABLE_PLATFORM, 0},
    {DeviceType::ELECTRICAL_HEATER, 1, NO_MODEL, "with adjustable setpoint", CAP_CLIMATE, SUBTYPE_PLATFORM,
     WINDOW_COMPANIONS},
    // A towel dryer reports the same 52/1 as the AGILIA radiator (two Atlantic Adelis units in issue
    // #202), so it is a model row: the YAML declares it (`io_model: towel_dryer`). It has no window
    // sensor and no open-window detection, and its own heating profile (proto_heating_profiles.cpp).
    {DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER, "towel_dryer", CAP_CLIMATE, SUBTYPE_PLATFORM, 0},
    // A heater with a night light (a Sauter Hekla, issue #222) reports the same 52/1 as well: everything the
    // radiator has, plus the light's registers in its own heating profile.
    {DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT, "heater_with_light", CAP_CLIMATE,
     SUBTYPE_PLATFORM, WINDOW_COMPANIONS | device_feature::NIGHT_LIGHT},

    {DeviceType::HEAT_RECOVERY_VENTILATION, 0, NO_MODEL, "heat_recovery_ventilation", CAP_NONE, 0, 0},

    {DeviceType::CENTRAL_HOUSE_CONTROL, 0, NO_MODEL, "central_house_control", CAP_NONE, 0, 0},

    {DeviceType::TEST_AND_EVALUATION, 0, NO_MODEL, "test_and_evaluation", CAP_NONE, 0, 0},

    {DeviceType::REMOTE_CONTROLLER, 0, NO_MODEL, "remote_controller", CAP_NONE, 0, 0},
    {DeviceType::REMOTE_CONTROLLER, 2, NO_MODEL, "Keygo", CAP_NONE, EXPERIMENTAL, 0},
    {DeviceType::REMOTE_CONTROLLER, 3, NO_MODEL, "Izymo", CAP_NONE, EXPERIMENTAL, 0},
    {DeviceType::REMOTE_CONTROLLER, 4, NO_MODEL, "public frame", CAP_NONE, EXPERIMENTAL, 0},
    {DeviceType::REMOTE_CONTROLLER, 5, NO_MODEL, "V500", CAP_NONE, EXPERIMENTAL, 0},
};

}  // namespace

const DeviceTypeInfo *find_exact_device_type_info(DeviceType type, uint8_t subtype) {
  for (const DeviceTypeInfo &row : DEVICE_TYPE_TABLE) {
    if (row.type == type && row.subtype == subtype && row.model == DeviceModel::NONE)
      return &row;
  }
  return nullptr;
}

const DeviceTypeInfo *find_device_model_info(DeviceType type, uint8_t subtype, DeviceModel model) {
  if (model == DeviceModel::NONE)
    return nullptr;
  for (const DeviceTypeInfo &row : DEVICE_TYPE_TABLE) {
    if (row.type == type && row.subtype == subtype && row.model == model)
      return &row;
  }
  return nullptr;
}

bool device_offers_models(DeviceType type, uint8_t subtype) {
  return std::any_of(std::begin(DEVICE_TYPE_TABLE), std::end(DEVICE_TYPE_TABLE),
                     [type, subtype](const DeviceTypeInfo &row) {
                       return row.type == type && row.subtype == subtype && row.model != DeviceModel::NONE;
                     });
}

const DeviceTypeInfo *find_device_type_info(DeviceType type, uint8_t subtype, DeviceModel model) {
  if (const DeviceTypeInfo *model_row = find_device_model_info(type, subtype, model))
    return model_row;
  const DeviceTypeInfo *row = find_exact_device_type_info(type, subtype);
  return row != nullptr || subtype == 0 ? row : find_exact_device_type_info(type, 0);
}

// The platform, the class and the experimental flag are facts of the (type, subtype): a model row keeps
// those of the row above it (device_types_table_test.cpp checks that), so these lookups take no model.

bool device_has_platform(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *row = find_device_type_info(type, subtype, DeviceModel::NONE);
  return row != nullptr && (row->flags & HAS_PLATFORM) != 0;
}

const char *device_platform_name(DeviceType type, uint8_t subtype) {
  if (!device_has_platform(type, subtype))
    return nullptr;
  switch (find_device_type_info(type, subtype, DeviceModel::NONE)->capability) {
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
    case DeviceCapabilityClass::WATER_HEATER:
      return "water_heater";
    // No platform drives these classes; listed so -Wswitch flags a new class that needs a decision.
    case DeviceCapabilityClass::UNKNOWN:
    case DeviceCapabilityClass::SENSOR:
    case DeviceCapabilityClass::BEACON:
      return nullptr;
  }
  return nullptr;
}

bool device_is_experimental(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *row = find_exact_device_type_info(type, subtype);
  return row != nullptr && (row->flags & device_type_flags::EXPERIMENTAL) != 0;
}

DeviceCapabilityClass device_capability_class(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *row = find_device_type_info(type, subtype, DeviceModel::NONE);
  return row != nullptr ? row->capability : DeviceCapabilityClass::UNKNOWN;
}

DeviceFeatures device_features(DeviceType type, uint8_t subtype, DeviceModel model) {
  if (const DeviceTypeInfo *model_row = find_device_model_info(type, subtype, model))
    return model_row->features;
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
    // No EXECUTE position path; listed so -Wswitch flags a new class that needs a decision. A water
    // heater must declare its kind (water_heater.py requires io_device_type), so it is never undeclared.
    case DeviceCapabilityClass::UNKNOWN:
    case DeviceCapabilityClass::SENSOR:
    case DeviceCapabilityClass::BEACON:
    case DeviceCapabilityClass::CLIMATE:
    case DeviceCapabilityClass::WATER_HEATER:
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
