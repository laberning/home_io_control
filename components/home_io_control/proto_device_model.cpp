/// @file proto_device_model.cpp
/// @brief Device-type capabilities, packed-metadata decoding and position reports.
/// @ingroup hioc_protocol

#include "proto_device_model.h"

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

/// Format a raw device type as hexadecimal, for diagnostics and as the YAML fallback.
std::string format_device_type_hex(DeviceType type) {
  char buf[DEVICE_TYPE_HEX_STRING_BUFFER_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned>(type));
  return std::string(buf);
}

}  // namespace

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

BatteryState decode_battery_state(uint8_t status_byte) {
  // The mask leaves exactly two bits, so every result is a named enumerator.
  return static_cast<BatteryState>((status_byte & STATUS_BATTERY_MASK) >> STATUS_BATTERY_SHIFT);
}

const char *cover_command_name(CoverCommand cmd) {
  switch (cmd) {
    case CoverCommand::STOP:
      return "STOP";
    case CoverCommand::FAVORITE:
      return "FAVORITE";
    case CoverCommand::VENT:
      return "VENT";
    case CoverCommand::FORCE_OPEN:
      return "FORCE_OPEN";
    default:
      return "UNKNOWN_COVER_CMD";
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

DeviceCapabilityClass device_capability_class(DeviceType type) { return device_capability_class(type, 0); }

const char *device_capability_class_name(DeviceType type) {
  switch (device_capability_class(type)) {
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
    case DeviceCapabilityClass::UNKNOWN:
      return "unknown";
  }
  return "unknown";
}

bool device_supports_position_control(DeviceType type) {
  return device_capability_class(type) == DeviceCapabilityClass::COVER;
}

bool device_supports_binary_control(DeviceType type) {
  DeviceCapabilityClass const capability_class = device_capability_class(type);
  return capability_class == DeviceCapabilityClass::LIGHT || capability_class == DeviceCapabilityClass::SWITCH;
}

bool device_supports_lock_control(DeviceType type) {
  return device_capability_class(type) == DeviceCapabilityClass::LOCK;
}

bool device_supports_heat_level_control(DeviceType type) {
  return device_capability_class(type) == DeviceCapabilityClass::HEATER_LEVEL;
}

bool device_has_no_motion(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype);
  // A heater commanded with an EXECUTE (levels, temperatures) never moves; a register heater is not
  // polled with 0x03 at all.
  return (profile != nullptr && profile->path != HeatingControlPath::PRIVATE_REGISTERS) ||
         device_supports_heat_level_control(type);
}

bool device_supports_climate_control(DeviceType type, uint8_t subtype) {
  return find_heating_profile(type, subtype) != nullptr;
}

bool device_supports_status_requests(DeviceType type) {
  return device_supports_position_control(type) || device_supports_binary_control(type) ||
         device_supports_lock_control(type) || device_supports_heat_level_control(type);
}

const char *device_operation_profile_name(DeviceType type, uint8_t subtype) {
  if (device_supports_position_control(type))
    return has_feature(device_features(type, subtype), device_feature::TILT) ? "cover_position_tilt" : "cover_position";
  if (device_supports_binary_control(type))
    return "binary_on_off";

  switch (device_capability_class(type)) {
    case DeviceCapabilityClass::LOCK:
      return "lock";
    case DeviceCapabilityClass::HEATER_LEVEL:
      return "heat_level";
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
    features = device_features(dev.type, dev.subtype);
  }
  // `dimmable:` has a schema default of false, so it decides a light's levels outright; the row's
  // LEVELS only drives the paste-ready snippet and the on/off-only rejection.
  if (entity_class == DeviceCapabilityClass::LIGHT) {
    features &= static_cast<DeviceFeatures>(~device_feature::LEVELS);
    if (dev.dimmable && !has_feature(features, device_feature::MAIN_PARAMETER_ONLY))
      features |= device_feature::LEVELS;
  }
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

std::string format_device_type_for_yaml(DeviceType type) {
  const char *name = yaml_device_type_name(type);
  if (name != nullptr) {
    return std::string("\"") + name + "\"";
  }
  return format_device_type_hex(type);
}

std::string build_device_yaml_snippet(DeviceType type, uint8_t subtype, const std::string &device_id,
                                      bool metadata_complete, bool low_power, const std::string &system_key_hex) {
  // A device that self-reported POWER_SAVE_LOW_POWER needs `low_power: true` so directed commands
  // wake it with the long preamble — emit it in both snippet shapes, since a low-power device that
  // also withheld its type is exactly the case a user most needs told.
  const std::string low_power_line = low_power ? "    low_power: true\n" : "";
  const std::string system_key_line = system_key_hex.empty() ? "" : "    system_key: \"" + system_key_hex + "\"\n";

  if (!metadata_complete) {
    return "  <cover|light|switch|lock>:\n"
           "  - platform: home_io_control\n"
           "    name: \"My Device\"\n"
           "    io_device_id: \"" +
           device_id +
           "\"\n"
           "    # io_device_type: left unset — this device didn't report its type during\n"
           "    #   discovery, so the controller learns it automatically from the next status\n"
           "    #   reply. Add it explicitly once you see it logged, to skip re-learning on\n"
           "    #   every future boot.\n" +
           low_power_line + system_key_line;
  }

  const char *platform = device_platform_name(type, subtype);
  if (platform == nullptr)
    return "";

  std::string extra_lines;
  const DeviceFeatures features = device_features(type, subtype);
  if (has_feature(features, device_feature::INVERTED))
    extra_lines += "    invert_position: true\n";
  if (device_capability_class(type, subtype) == DeviceCapabilityClass::LIGHT &&
      has_feature(features, device_feature::LEVELS))
    extra_lines += "    dimmable: true\n";
  extra_lines += low_power_line;
  extra_lines += system_key_line;

  const std::string subtype_line = "    io_subtype: " + std::to_string(subtype) + "\n";

  return "  " + std::string(platform) +
         ":\n"
         "  - platform: home_io_control\n"
         "    name: \"My Device\"\n"
         "    io_device_id: \"" +
         device_id + "\"\n" + "    io_device_type: " + format_device_type_for_yaml(type) + "\n" + subtype_line +
         extra_lines;
}

}  // namespace home_io_control
}  // namespace esphome
