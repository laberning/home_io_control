/// @file proto_device_model.cpp
/// @brief Device-type capabilities, packed-metadata decoding and position reports.
/// @ingroup hioc_protocol

#include "proto_device_model.h"

#include <cmath>

namespace esphome {
namespace home_io_control {

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
    default:
      return nullptr;
  }
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
    default:
      return DeviceCapabilityClass::UNKNOWN;
  }
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
    default:
      return "unknown";
  }
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
  switch (type) {
    case DeviceType::VENETIAN_BLIND:
    case DeviceType::BLIND:
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
      return true;
    default:
      return false;
  }
}

bool device_supports_vent(DeviceType type) {
  switch (type) {
    case DeviceType::WINDOW_OPENER:
    case DeviceType::VENTILATION_POINT:
      return true;
    default:
      return false;
  }
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
    case DeviceCapabilityClass::UNKNOWN:
    default:
      return "unknown";
  }
}

}  // namespace home_io_control
}  // namespace esphome
