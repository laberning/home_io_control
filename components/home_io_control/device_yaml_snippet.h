#pragma once

/// @file device_yaml_snippet.h
/// @brief The ESPHome YAML a user pastes after a device is paired or discovered.
/// @ingroup hioc_hub
///
/// Knowledge of the Home Assistant integration's configuration: which ESPHome platform drives a
/// device kind and which keys its entry carries. The protocol layer knows nothing of it; this file reads
/// the node-type table through proto_device_types.h.

#include "proto_device_kinds.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Name of the ESPHome platform that drives this (type, subtype).
/// @param type Node type.
/// @param subtype Subtype.
/// @return "cover", "light", "switch", "lock", "climate", "fan" or "water_heater", or nullptr when no platform
///         drives it yet.
const char *device_platform_name(DeviceType type, uint8_t subtype);

/// @brief Build the YAML value for a device's `io_device_type` key.
///
/// The one rule for how a type is written in a config: the name the Python schema accepts when the
/// type has one, otherwise the raw hex value. The pairing snippet, the one-way adoption report and
/// the boot-time hint all use it.
/// @param type Device type.
/// @return A quoted symbolic name (e.g. `"horizontal_awning"`) when one exists, otherwise the
///         raw hex form (e.g. `0x1A`, `0x3FF`) for a type with no YAML alias.
std::string format_device_type_for_yaml(DeviceType type);

/// @brief Build the ready-to-paste YAML block describing a device, for both a fully-decoded
/// device and one whose type/subtype wasn't reported.
///
/// With `metadata_complete == false`, the type/subtype are unknown, so the returned snippet
/// uses a `<cover|light|switch|lock>` platform placeholder and a commented-out
/// `io_device_type` explanation; `type` and `subtype` are ignored in this case.
///
/// With `metadata_complete == true`, the snippet names the concrete ESPHome platform for
/// `type` and fills in `io_subtype`, `invert_position: true` for a kind inverted by default and
/// `dimmable: true` for a light subtype with intermediate levels (device_feature::INVERTED and
/// device_feature::LEVELS of the kind). If the (type, subtype) has no ESPHome
/// platform yet (device_has_platform() in proto_device_types.h), an empty string is returned
/// instead — the caller decides what to say when there's no snippet to show.
///
/// `low_power: true` is emitted in **both** shapes when `low_power` is set (the device
/// self-reported POWER_SAVE_LOW_POWER in its discovery Multi Information Byte); it is omitted
/// entirely otherwise, since its absence is a valid, correct config.
///
/// The emitted keys must track the device-bound platform schemas (`platform_schema_extension()`
/// in platform_common.py, plus each of cover.py/light.py/switch.py/lock.py's own extra keys) by
/// hand. `make yaml-emitter-sync` (scripts/check-yaml-emitters.py) catches drift between the two
/// statically.
/// @param type Decoded device type.
/// @param subtype Decoded device subtype; only used when `metadata_complete` is true.
/// @param device_id Hex device ID string (e.g. "38B4A1").
/// @param metadata_complete Whether the discovery response included type/subtype metadata.
/// @param low_power Whether the device self-reported a low-power / duty-cycled class; emits
///        `low_power: true` when set, in both snippet shapes.
/// @param system_key_hex The device's own system key as 32 hex characters, or empty when it uses the
///        hub's. Emits `system_key: "<hex>"` in both snippet shapes. Key material: a caller that
///        passes one must print the snippet only inside a DO-NOT-SHARE block.
/// @return Multi-line YAML snippet, or an empty string when `metadata_complete` is true but
///         the (type, subtype) maps to no ESPHome platform.
std::string build_device_yaml_snippet(DeviceType type, uint8_t subtype, const std::string &device_id,
                                      bool metadata_complete, bool low_power,
                                      const std::string &system_key_hex = std::string());

}  // namespace home_io_control
}  // namespace esphome
