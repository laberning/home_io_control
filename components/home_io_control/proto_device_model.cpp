/// @file proto_device_model.cpp
/// @brief Device-type capabilities, packed-metadata decoding and position reports.
/// @ingroup hioc_protocol

#include "proto_device_model.h"

#include "proto_device_kinds.h"
#include "proto_heating_profiles.h"

#include "proto_constants.h"
#include "proto_device_types.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

constexpr size_t DEVICE_TYPE_HEX_STRING_BUFFER_SIZE = 8;  ///< Buffer for strings such as "0x3FF" plus terminator.

}  // namespace

std::string format_device_type_hex(DeviceType type) {
  char buf[DEVICE_TYPE_HEX_STRING_BUFFER_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned>(type));
  return std::string(buf);
}

const char *rain_sensor_state_name(RainSensorState state) {
  switch (state) {
    case RainSensorState::UNKNOWN:
      return "unknown";
    case RainSensorState::DRY:
      return "dry";
    case RainSensorState::RAIN:
      return "rain";
  }
  return "unknown";
}

ExecutionState execution_state_of(uint8_t status_byte) {
  const uint8_t state = error_resp_state(status_byte);
  return state <= static_cast<uint8_t>(ExecutionState::DONE) ? static_cast<ExecutionState>(state)
                                                             : ExecutionState::UNDEFINED;
}

uint8_t result_code_of(uint8_t status_byte) { return error_resp_code(status_byte); }

const char *execution_state_name(ExecutionState state) {
  switch (state) {
    case ExecutionState::NON_EXECUTING:
      return "non_executing";
    case ExecutionState::EXECUTION_ERROR:
      return "error";
    case ExecutionState::WAITING_FOR_EXECUTING:
      return "waiting_for_executing";
    case ExecutionState::WAITING_FOR_POWER:
      return "waiting_for_power";
    case ExecutionState::EXECUTING:
      return "executing";
    case ExecutionState::DONE:
      return "done";
    case ExecutionState::UNDEFINED:
    default:
      return "undefined";
  }
}

const char *battery_state_name(BatteryState state) {
  switch (state) {
    case BatteryState::NOT_REPORTED:
      return "not_reported";
    case BatteryState::LOW_CHARGE:
      return "low";
    case BatteryState::NORMAL_CHARGE:
      return "normal";
    case BatteryState::FULL_CHARGE:
      return "full";
  }
  return "not_reported";
}

const char *travel_speed_name(TravelSpeed speed) {
  switch (speed) {
    case TravelSpeed::MOTOR_DEFAULT:
      return "default";
    case TravelSpeed::SILENT:
      return "silent";
    case TravelSpeed::FAST:
      return "fast";
  }
  return "fast";
}

bool parse_travel_speed(const std::string &name, TravelSpeed &speed) {
  for (const TravelSpeed candidate : {TravelSpeed::MOTOR_DEFAULT, TravelSpeed::SILENT, TravelSpeed::FAST}) {
    if (name == travel_speed_name(candidate)) {
      speed = candidate;
      return true;
    }
  }
  return false;
}

BatteryState decode_battery_state(uint8_t status_byte) {
  // The mask leaves exactly two bits, so every result is a named enumerator.
  return static_cast<BatteryState>((status_byte & STATUS_BATTERY_MASK) >> STATUS_BATTERY_SHIFT);
}

const char *cover_command_name(CoverCommand cmd) {
  switch (cmd) {
    case CoverCommand::STOP:
      return "STOP";
    case CoverCommand::FORCE_OPEN:
      return "FORCE_OPEN";
    default: {
      const NamedPosition *named = find_named_position(cmd);
      return named != nullptr ? named->name : "UNKNOWN_COVER_CMD";
    }
  }
}

DeviceType decode_packed_device_type(uint8_t type_msb, uint8_t type_subtype) {
  return static_cast<DeviceType>((type_msb << DEVICE_TYPE_LOW_BITS_SHIFT) |
                                 (type_subtype >> DEVICE_TYPE_HIGH_BITS_SHIFT));
}

uint8_t decode_packed_device_subtype(uint8_t type_subtype) { return type_subtype & DEVICE_SUBTYPE_MASK; }

void encode_packed_device_type(DeviceType type, uint8_t subtype, uint8_t &type_msb, uint8_t &type_subtype) {
  // The wire field holds ten type bits; anything above is dropped here rather than aliasing the
  // subtype bits below.
  const auto raw = static_cast<uint16_t>(static_cast<uint16_t>(type) & DEVICE_TYPE_WIRE_MASK);
  type_msb = static_cast<uint8_t>(raw >> DEVICE_TYPE_LOW_BITS_SHIFT);
  type_subtype = static_cast<uint8_t>((raw << DEVICE_TYPE_HIGH_BITS_SHIFT) | (subtype & DEVICE_SUBTYPE_MASK));
}

void decode_position_report(uint16_t target_raw, uint16_t current_raw, bool is_stopped, float &target,
                            float &position) {
  bool const target_valid = target_raw <= STATUS_POS_MAX;
  bool const current_valid = current_raw <= STATUS_POS_MAX;
  float const decoded_current = current_valid ? current_raw * 100.0F / STATUS_POS_MAX : UNKNOWN_POSITION;

  if (target_valid) {
    target = target_raw * 100.0F / STATUS_POS_MAX;
  } else if (is_stopped && current_valid) {
    // Marker values such as D2 (stop) and D4 (keep position during tilt) exceed STATUS_POS_MAX.
    // When the device says it is stopped and still gives a valid current position, use that as
    // the effective target instead of discarding the target entirely.
    target = decoded_current;
  } else {
    target = UNKNOWN_POSITION;
  }

  if (current_valid) {
    position = decoded_current;
  } else if (is_stopped && target_valid) {
    position = target;
  } else {
    position = UNKNOWN_POSITION;
  }
}

bool has_reached_target_position(float target, float position) {
  if (target == UNKNOWN_POSITION || position == UNKNOWN_POSITION)
    return false;
  float const tolerance = STATUS_POS_TOLERANCE_RAW * 100.0F / STATUS_POS_MAX;
  return std::fabs(target - position) <= tolerance;
}

float decode_tilt_report(uint16_t tilt_raw) {
  if (tilt_raw > STATUS_POS_MAX)
    return UNKNOWN_POSITION;
  return 100.0F - (tilt_raw * 100.0F / STATUS_POS_MAX);
}

bool is_known_device_type(uint16_t raw) {
  return raw != static_cast<uint16_t>(DeviceType::UNKNOWN) &&
         find_exact_device_type_info(static_cast<DeviceType>(raw), 0) != nullptr;
}

const char *device_type_name(DeviceType type) {
  const DeviceTypeInfo *row = find_exact_device_type_info(type, 0);
  return row != nullptr ? row->name : "unknown";
}

const char *yaml_device_type_name(DeviceType type) {
  const DeviceTypeInfo *row = find_exact_device_type_info(type, 0);
  return row != nullptr && (row->flags & device_type_flags::YAML_SELECTABLE) != 0 ? row->name : nullptr;
}

const char *device_capability_class_name(DeviceType type, uint8_t subtype) {
  switch (device_capability_class(type, subtype)) {
    case DeviceCapabilityClass::COVER:
      return "cover";
    case DeviceCapabilityClass::LIGHT:
      return "light";
    case DeviceCapabilityClass::SWITCH:
      return "switch";
    case DeviceCapabilityClass::SENSOR:
      return "sensor";
    case DeviceCapabilityClass::BEACON:
      return "beacon";
    case DeviceCapabilityClass::CLIMATE:
      return "climate";
    case DeviceCapabilityClass::LOCK:
      return "lock";
    case DeviceCapabilityClass::HEATER_LEVEL:
      return "heater_level";
    case DeviceCapabilityClass::WATER_HEATER:
      return "water_heater";
    case DeviceCapabilityClass::UNKNOWN:
      return "unknown";
  }
  return "unknown";
}

// === Single-class predicates (file-local) ===

namespace {

bool is_position_control(DeviceType type, uint8_t subtype) {
  return device_capability_class(type, subtype) == DeviceCapabilityClass::COVER;
}

bool is_binary_control(DeviceType type, uint8_t subtype) {
  DeviceCapabilityClass const capability_class = device_capability_class(type, subtype);
  return capability_class == DeviceCapabilityClass::LIGHT || capability_class == DeviceCapabilityClass::SWITCH;
}

bool is_lock_control(DeviceType type, uint8_t subtype) {
  return device_capability_class(type, subtype) == DeviceCapabilityClass::LOCK;
}

bool is_heat_level_control(DeviceType type, uint8_t subtype) {
  return device_capability_class(type, subtype) == DeviceCapabilityClass::HEATER_LEVEL;
}

}  // namespace

bool device_has_no_motion(DeviceType type, uint8_t subtype) {
  // The control path is a fact of the (type, subtype); a model's profile keeps it.
  const HeatingProfile *profile = find_heating_profile(type, subtype, DeviceModel::NONE);
  // A heater commanded with an EXECUTE (levels, temperatures) never moves; a register heater is not
  // polled with 0x03 at all. A water heater answers a status read but never moves either.
  return (profile != nullptr && profile->path != HeatingControlPath::PRIVATE_REGISTERS) ||
         is_heat_level_control(type, subtype) ||
         has_feature(device_features(type, subtype, DeviceModel::NONE), device_feature::WATER_HEATER);
}

bool device_supports_climate_control(DeviceType type, uint8_t subtype) {
  // A model never adds or removes a profile: it only replaces the (type, subtype) one.
  return find_heating_profile(type, subtype, DeviceModel::NONE) != nullptr;
}

bool device_takes_execute_position(DeviceType type, uint8_t subtype) {
  return is_position_control(type, subtype) || is_binary_control(type, subtype) || is_lock_control(type, subtype) ||
         is_heat_level_control(type, subtype);
}

const char *device_operation_profile_name(DeviceType type, uint8_t subtype) {
  if (is_position_control(type, subtype)) {
    // A cover has no model rows; the operation profile names the wire type.
    const DeviceFeatures features = device_features(type, subtype, DeviceModel::NONE);
    if (!has_feature(features, device_feature::LEVELS))
      return "cover_discrete";
    return has_feature(features, device_feature::TILT) ? "cover_position_tilt" : "cover_position";
  }
  if (is_binary_control(type, subtype))
    return "binary_on_off";

  switch (device_capability_class(type, subtype)) {
    case DeviceCapabilityClass::LOCK:
      return "lock";
    case DeviceCapabilityClass::HEATER_LEVEL:
      return "heat_level";
    case DeviceCapabilityClass::WATER_HEATER:
      return "water_heater";
    case DeviceCapabilityClass::CLIMATE:
      return "climate";
    case DeviceCapabilityClass::SENSOR:
      return "sensor";
    case DeviceCapabilityClass::BEACON:
      return "beacon";
    // COVER/LIGHT/SWITCH are handled by the position/binary guards above and never reach here;
    // listed explicitly (no default:) so -Wswitch covers any new capability class.
    case DeviceCapabilityClass::COVER:
    case DeviceCapabilityClass::LIGHT:
    case DeviceCapabilityClass::SWITCH:
    case DeviceCapabilityClass::UNKNOWN:
      return "unknown";
  }
  return "unknown";
}

void resolve_device_features(IoDevice &dev) {
  const DeviceCapabilityClass entity_class = dev.entity_class != DeviceCapabilityClass::UNKNOWN
                                                 ? dev.entity_class
                                                 : device_capability_class(dev.type, dev.subtype);
  DeviceFeatures features;
  if (dev.type == DeviceType::UNKNOWN || find_exact_device_type_info(dev.type, 0) == nullptr) {
    features = undeclared_kind_features(entity_class);
    if (dev.subtype == SUBTYPE_MAIN_PARAMETER_ONLY)
      features = main_parameter_only_variant(features);
  } else {
    features = device_features(dev.type, dev.subtype, dev.model);
  }
  // `dimmable:` has a schema default of false, so it decides a light's levels outright; the row's
  // LEVELS only drives the paste-ready snippet and the on/off-only rejection.
  if (entity_class == DeviceCapabilityClass::LIGHT) {
    features = features.without(device_feature::LEVELS);
    if (dev.dimmable && !has_feature(features, device_feature::MAIN_PARAMETER_ONLY))
      features |= device_feature::LEVELS;
  }
  // The water heater path (its poll, its register reads and writes) belongs to a water heater entity. A
  // device of that kind bound to another platform (a `switch:` some configs declare only to reach the
  // register probes) keeps none of it, so its frames stay what they were before that path existed.
  if (entity_class != DeviceCapabilityClass::WATER_HEATER)
    features = features.without(device_feature::WATER_HEATER);
  dev.features = features;
  dev.inverted = dev.invert_override.value_or(has_feature(features, device_feature::INVERTED));
}

std::string format_device_type_diagnostic(DeviceType type) {
  const char *name = device_type_name(type);
  std::string raw = format_device_type_hex(type);
  if (name != nullptr && strcmp(name, "unknown") != 0) {
    return std::string(name) + " (" + raw + ")";
  }
  return raw;
}

}  // namespace home_io_control
}  // namespace esphome
