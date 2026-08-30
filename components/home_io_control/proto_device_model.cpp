/// @file proto_device_model.cpp
/// @brief Device-type capabilities, packed-metadata decoding and position reports.
/// @ingroup hioc_protocol

#include "proto_device_model.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

constexpr size_t DEVICE_TYPE_HEX_STRING_BUFFER_SIZE = 8;  ///< Buffer for strings such as "0x11" plus terminator.

/// Format a raw device type as hexadecimal, for diagnostics and as the YAML fallback.
std::string format_device_type_hex(DeviceType type) {
  char buf[DEVICE_TYPE_HEX_STRING_BUFFER_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X", static_cast<uint8_t>(type));
  return std::string(buf);
}

/// Map a capability class to the corresponding ESPHome platform name, or nullptr when the
/// class has no dedicated platform yet.
const char *pairing_platform_name(DeviceCapabilityClass capability_class) {
  switch (capability_class) {
    case DeviceCapabilityClass::COVER:
      return "cover";
    case DeviceCapabilityClass::LIGHT:
      return "light";
    case DeviceCapabilityClass::SWITCH:
      return "switch";
    case DeviceCapabilityClass::LOCK:
      return "lock";
    default:
      return nullptr;
  }
}

}  // namespace

bool default_inverted_for_type(DeviceType type) { return type == DeviceType::HORIZONTAL_AWNING; }

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
  const auto raw = static_cast<uint8_t>(type);
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

const char *device_type_name(DeviceType type) {
  switch (type) {
    case DeviceType::UNKNOWN:
      return "unknown";
    case DeviceType::VENETIAN_BLIND:
      return "venetian_blind";
    case DeviceType::ROLLER_SHUTTER:
      return "roller_shutter";
    case DeviceType::SCREEN:
      return "screen";
    case DeviceType::AWNING:
      return "awning";
    case DeviceType::WINDOW_OPENER:
      return "window_opener";
    case DeviceType::GARAGE_OPENER:
      return "garage_opener";
    case DeviceType::LIGHT:
      return "light";
    case DeviceType::GATE_OPENER:
      return "gate_opener";
    case DeviceType::ROLLING_DOOR_OPENER:
      return "rolling_door_opener";
    case DeviceType::BLIND:
      return "blind";
    case DeviceType::DUAL_SHUTTER:
      return "dual_shutter";
    case DeviceType::ON_OFF_SWITCH:
      return "on_off_switch";
    case DeviceType::HORIZONTAL_AWNING:
      return "horizontal_awning";
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
      return "external_venetian_blind";
    case DeviceType::LOUVRE_BLIND:
      return "louvre_blind";
    case DeviceType::CURTAIN_TRACK:
      return "curtain_track";
    case DeviceType::SWINGING_SHUTTER:
      return "swinging_shutter";
    case DeviceType::LOCK:
      return "lock";
    case DeviceType::BEACON:
      return "beacon";
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
      return "heating_temperature_interface";
    case DeviceType::VENTILATION_POINT:
      return "ventilation_point";
    case DeviceType::EXTERIOR_HEATING:
      return "exterior_heating";
    case DeviceType::HEAT_PUMP:
      return "heat_pump";
    case DeviceType::INTRUSION_ALARM:
      return "intrusion_alarm";
  }

  return "unknown";
}

const char *yaml_device_type_name(DeviceType type) {
  switch (type) {
    case DeviceType::UNKNOWN:
      return "unknown";
    case DeviceType::VENETIAN_BLIND:
      return "venetian_blind";
    case DeviceType::ROLLER_SHUTTER:
      return "roller_shutter";
    case DeviceType::AWNING:
      return "awning";
    case DeviceType::WINDOW_OPENER:
      return "window_opener";
    case DeviceType::GARAGE_OPENER:
      return "garage_opener";
    case DeviceType::LIGHT:
      return "light";
    case DeviceType::GATE_OPENER:
      return "gate_opener";
    case DeviceType::ROLLING_DOOR_OPENER:
      return "rolling_door_opener";
    case DeviceType::LOCK:
      return "lock";
    case DeviceType::BLIND:
      return "blind";
    case DeviceType::SCREEN:
      return "screen";
    case DeviceType::DUAL_SHUTTER:
      return "dual_shutter";
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
      return "heating_temperature_interface";
    case DeviceType::ON_OFF_SWITCH:
      return "on_off_switch";
    case DeviceType::HORIZONTAL_AWNING:
      return "horizontal_awning";
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
      return "external_venetian_blind";
    case DeviceType::LOUVRE_BLIND:
      return "louvre_blind";
    case DeviceType::CURTAIN_TRACK:
      return "curtain_track";
    case DeviceType::INTRUSION_ALARM:
      return "intrusion_alarm";
    case DeviceType::SWINGING_SHUTTER:
      return "swinging_shutter";
    // Not YAML-selectable (nullptr, so callers fall back to a raw numeric value). Listed
    // explicitly instead of default: so -Wswitch flags any new DeviceType that skips this switch.
    case DeviceType::BEACON:
    case DeviceType::VENTILATION_POINT:
    case DeviceType::EXTERIOR_HEATING:
    case DeviceType::HEAT_PUMP:
      return nullptr;
  }
  return nullptr;
}

DeviceCapabilityClass device_capability_class(DeviceType type) {
  switch (type) {
    // Cover types (position-controlled)
    case DeviceType::VENETIAN_BLIND:
    case DeviceType::ROLLER_SHUTTER:
    case DeviceType::SCREEN:
    case DeviceType::AWNING:
    case DeviceType::WINDOW_OPENER:
    case DeviceType::GARAGE_OPENER:
    case DeviceType::GATE_OPENER:
    case DeviceType::ROLLING_DOOR_OPENER:
    case DeviceType::BLIND:
    case DeviceType::DUAL_SHUTTER:
    case DeviceType::HORIZONTAL_AWNING:
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
    case DeviceType::CURTAIN_TRACK:
    case DeviceType::SWINGING_SHUTTER:
      return DeviceCapabilityClass::COVER;

    // Binary and other capabilities
    case DeviceType::LIGHT:
      return DeviceCapabilityClass::LIGHT;
    case DeviceType::ON_OFF_SWITCH:
      return DeviceCapabilityClass::SWITCH;
    case DeviceType::LOCK:
      return DeviceCapabilityClass::LOCK;
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
    case DeviceType::EXTERIOR_HEATING:
    case DeviceType::HEAT_PUMP:
      return DeviceCapabilityClass::CLIMATE;
    case DeviceType::VENTILATION_POINT:
      // Binary ventilation on/off; treated as switch
      return DeviceCapabilityClass::SWITCH;
    case DeviceType::BEACON:
      return DeviceCapabilityClass::BEACON;
    case DeviceType::INTRUSION_ALARM:
      return DeviceCapabilityClass::SENSOR;

    case DeviceType::UNKNOWN:
      return DeviceCapabilityClass::UNKNOWN;
  }
  // No default: -Wswitch flags any new DeviceType not handled above (a hard error in the host unit
  // build via -Werror=switch, a warning in the firmware build), rather than letting it fall through
  // silently to UNKNOWN (which would make known_device_matches_entity_class() accept everything for
  // that type).
  return DeviceCapabilityClass::UNKNOWN;
}

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

bool device_supports_status_requests(DeviceType type) {
  return device_supports_position_control(type) || device_supports_binary_control(type) ||
         device_supports_lock_control(type);
}

bool device_supports_tilt(DeviceType type) {
  // No default: every DeviceType is listed so -Wswitch catches any new one (error in the host
  // unit build, warning in the firmware build), forcing an explicit tilt/vent decision.
  switch (type) {
    case DeviceType::VENETIAN_BLIND:
    case DeviceType::BLIND:
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
      return true;
    case DeviceType::UNKNOWN:
    case DeviceType::ROLLER_SHUTTER:
    case DeviceType::AWNING:
    case DeviceType::WINDOW_OPENER:
    case DeviceType::GARAGE_OPENER:
    case DeviceType::LIGHT:
    case DeviceType::GATE_OPENER:
    case DeviceType::ROLLING_DOOR_OPENER:
    case DeviceType::LOCK:
    case DeviceType::SCREEN:
    case DeviceType::BEACON:
    case DeviceType::DUAL_SHUTTER:
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
    case DeviceType::ON_OFF_SWITCH:
    case DeviceType::HORIZONTAL_AWNING:
    case DeviceType::CURTAIN_TRACK:
    case DeviceType::VENTILATION_POINT:
    case DeviceType::EXTERIOR_HEATING:
    case DeviceType::HEAT_PUMP:
    case DeviceType::INTRUSION_ALARM:
    case DeviceType::SWINGING_SHUTTER:
      return false;
  }
  return false;
}

bool device_supports_vent(DeviceType type) {
  // No default: every DeviceType is listed so -Wswitch catches any new one (error in the host
  // unit build, warning in the firmware build), forcing an explicit tilt/vent decision.
  switch (type) {
    case DeviceType::WINDOW_OPENER:
    case DeviceType::VENTILATION_POINT:
      return true;
    case DeviceType::UNKNOWN:
    case DeviceType::VENETIAN_BLIND:
    case DeviceType::ROLLER_SHUTTER:
    case DeviceType::AWNING:
    case DeviceType::GARAGE_OPENER:
    case DeviceType::LIGHT:
    case DeviceType::GATE_OPENER:
    case DeviceType::ROLLING_DOOR_OPENER:
    case DeviceType::LOCK:
    case DeviceType::BLIND:
    case DeviceType::SCREEN:
    case DeviceType::BEACON:
    case DeviceType::DUAL_SHUTTER:
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
    case DeviceType::ON_OFF_SWITCH:
    case DeviceType::HORIZONTAL_AWNING:
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
    case DeviceType::CURTAIN_TRACK:
    case DeviceType::EXTERIOR_HEATING:
    case DeviceType::HEAT_PUMP:
    case DeviceType::INTRUSION_ALARM:
    case DeviceType::SWINGING_SHUTTER:
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
                                      bool metadata_complete, bool inverted, bool low_power) {
  // A device that self-reported POWER_SAVE_LOW_POWER needs `low_power: true` so directed commands
  // wake it with the long preamble — emit it in both snippet shapes, since a low-power device that
  // also withheld its type is exactly the case a user most needs told.
  const std::string low_power_line = low_power ? "    low_power: true\n" : "";

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
           low_power_line;
  }

  const auto capability_class = device_capability_class(type);
  const char *platform = pairing_platform_name(capability_class);
  if (platform == nullptr)
    return "";

  std::string extra_lines;
  if (capability_class == DeviceCapabilityClass::COVER && inverted)
    extra_lines += "    invert_position: true\n";
  extra_lines += low_power_line;

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
