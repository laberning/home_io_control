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

bool default_inverted_for_type(DeviceType type) { return type == DeviceType::HORIZONTAL_AWNING; }

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
  return device_uses_heating_levels(type, subtype) || device_supports_heat_level_control(type);
}

bool device_supports_climate_control(DeviceType type, uint8_t subtype) {
  return find_heating_profile(type, subtype) != nullptr;
}

bool device_supports_status_requests(DeviceType type) {
  return device_supports_position_control(type) || device_supports_binary_control(type) ||
         device_supports_lock_control(type) || device_supports_heat_level_control(type);
}

bool device_supports_tilt(DeviceType type) {
  // No default: every DeviceType is listed so -Wswitch catches any new one (error in the host
  // unit build, warning in the firmware build), forcing an explicit tilt/vent decision.
  switch (type) {
    case DeviceType::INTERIOR_VENETIAN_BLIND:
    case DeviceType::VERTICAL_INTERIOR_BLIND:
    case DeviceType::EXTERIOR_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
      return true;
    case DeviceType::UNKNOWN:
    case DeviceType::ROLLER_SHUTTER:
    case DeviceType::VERTICAL_EXTERIOR_AWNING:
    case DeviceType::WINDOW_OPENER:
    case DeviceType::GARAGE_OPENER:
    case DeviceType::LIGHT:
    case DeviceType::GATE_OPENER:
    case DeviceType::ROLLING_DOOR_OPENER:
    case DeviceType::LOCK:
    case DeviceType::SECURE_CONFIGURATION_DEVICE:
    case DeviceType::BEACON:
    case DeviceType::DUAL_ROLLER_SHUTTER:
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
    case DeviceType::ON_OFF_SWITCH:
    case DeviceType::HORIZONTAL_AWNING:
    case DeviceType::CURTAIN_TRACK:
    case DeviceType::VENTILATION_POINT:
    case DeviceType::EXTERIOR_HEATING:
    case DeviceType::HEAT_PUMP:
    case DeviceType::INTRUSION_ALARM:
    case DeviceType::SWINGING_SHUTTER:
    case DeviceType::SLIDING_WINDOW:
    case DeviceType::ZONE_CONTROL:
    case DeviceType::BIOCLIMATIC_PERGOLA:
    case DeviceType::SIREN:
    case DeviceType::DOMESTIC_HOT_WATER:
    case DeviceType::ELECTRICAL_HEATER:
    case DeviceType::HEAT_RECOVERY_VENTILATION:
    case DeviceType::CENTRAL_HOUSE_CONTROL:
    case DeviceType::TEST_AND_EVALUATION:
    case DeviceType::REMOTE_CONTROLLER:
      return false;
  }
  return false;
}

bool device_takes_main_parameter_only(DeviceType type, uint8_t subtype) {
  if (subtype == SUBTYPE_MAIN_PARAMETER_ONLY)
    return true;
  const DeviceTypeInfo *row = find_device_type_info(type, subtype);
  return row != nullptr && (row->flags & device_type_flags::MAIN_PARAMETER_ONLY) != 0;
}

bool device_supports_vent(DeviceType type) {
  // No default: every DeviceType is listed so -Wswitch catches any new one (error in the host
  // unit build, warning in the firmware build), forcing an explicit tilt/vent decision.
  switch (type) {
    case DeviceType::WINDOW_OPENER:
    case DeviceType::VENTILATION_POINT:
      return true;
    case DeviceType::UNKNOWN:
    case DeviceType::INTERIOR_VENETIAN_BLIND:
    case DeviceType::ROLLER_SHUTTER:
    case DeviceType::VERTICAL_EXTERIOR_AWNING:
    case DeviceType::GARAGE_OPENER:
    case DeviceType::LIGHT:
    case DeviceType::GATE_OPENER:
    case DeviceType::ROLLING_DOOR_OPENER:
    case DeviceType::LOCK:
    case DeviceType::VERTICAL_INTERIOR_BLIND:
    case DeviceType::SECURE_CONFIGURATION_DEVICE:
    case DeviceType::BEACON:
    case DeviceType::DUAL_ROLLER_SHUTTER:
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
    case DeviceType::ON_OFF_SWITCH:
    case DeviceType::HORIZONTAL_AWNING:
    case DeviceType::EXTERIOR_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
    case DeviceType::CURTAIN_TRACK:
    case DeviceType::EXTERIOR_HEATING:
    case DeviceType::HEAT_PUMP:
    case DeviceType::INTRUSION_ALARM:
    case DeviceType::SWINGING_SHUTTER:
    case DeviceType::SLIDING_WINDOW:
    case DeviceType::ZONE_CONTROL:
    case DeviceType::BIOCLIMATIC_PERGOLA:
    case DeviceType::SIREN:
    case DeviceType::DOMESTIC_HOT_WATER:
    case DeviceType::ELECTRICAL_HEATER:
    case DeviceType::HEAT_RECOVERY_VENTILATION:
    case DeviceType::CENTRAL_HOUSE_CONTROL:
    case DeviceType::TEST_AND_EVALUATION:
    case DeviceType::REMOTE_CONTROLLER:
      return false;
  }
  return false;
}

const char *device_operation_profile_name(DeviceType type) {
  if (device_supports_position_control(type))
    return device_supports_tilt(type) ? "cover_position_tilt" : "cover_position";
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
                                      bool metadata_complete, bool inverted, bool low_power,
                                      const std::string &system_key_hex) {
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
  if (device_capability_class(type, subtype) == DeviceCapabilityClass::COVER && inverted)
    extra_lines += "    invert_position: true\n";
  if (device_light_is_brightness_only(type, subtype))
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
