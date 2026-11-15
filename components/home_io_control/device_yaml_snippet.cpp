/// @file device_yaml_snippet.cpp
/// @brief Platform names and the paste-ready YAML snippet for a device.
/// @ingroup hioc_hub

#include "device_yaml_snippet.h"

#include "proto_device_features.h"
#include "proto_device_kinds.h"
#include "proto_device_model.h"
#include "proto_device_types.h"

namespace esphome {
namespace home_io_control {

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

std::string format_device_type_for_yaml(DeviceType type) {
  const char *name = yaml_device_type_name(type);
  if (name != nullptr)
    return std::string("\"") + name + "\"";
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
  // The snippet is built from what the device reported, which carries no model.
  const DeviceFeatures features = device_features(type, subtype, DeviceModel::NONE);
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
