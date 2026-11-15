#pragma once

/// @file proto_device_features.h
/// @brief Capability bits of a device kind: what a (node type, subtype) can do.
/// @ingroup hioc_protocol
///
/// The bits are carried by the rows of the node-type table (proto_device_types.h) and resolved onto
/// each device record, so the hub, the platforms and the Python codegen ask one place whether a
/// device has a feature instead of branching on a device type. A bit says *whether* a device has a
/// capability; how it is commanded stays in the command builders and the profile tables.
///
/// This leaf header includes only `<cstdint>`: IoDevice (proto_device_model.h) carries a
/// DeviceFeatures word, and so does every row of the table in proto_device_types.h.

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Capability bits of a device kind; see device_feature.
using DeviceFeatures = uint16_t;

/// @brief Capability bits carried by a DeviceTypeInfo row and resolved onto a device record.
/// hub_validators.py mirrors the whole word bit for bit (`FEATURE_*`, `DEVICE_FEATURES`) and
/// device_features_sync_test.cpp checks it. Bit 15 is never a capability: the Python mirror uses it
/// for the row's experimental marker.
namespace device_feature {
/// EXECUTE carries no functional parameter at all (VELUX KLF 200 API specification, Appendix 2:
/// "main parameter only"). Read by main_parameter_only_variant() and the table invariants; the frame
/// shape itself is EXTENDED_POSITION_FRAME's job.
static constexpr DeviceFeatures MAIN_PARAMETER_ONLY = 1U << 0U;
/// Intermediate main-parameter values: a percent position, a brightness or a heat level.
static constexpr DeviceFeatures LEVELS = 1U << 1U;
/// Slat angle (functional parameter 3) through the extended status and the tilt EXECUTE.
static constexpr DeviceFeatures TILT = 1U << 2U;
/// Named position 0xD800, the favourite ("my") position. Absent where a device has refused the alias
/// (garage openers, issue #163).
static constexpr DeviceFeatures FAVORITE = 1U << 3U;
/// Named position 0xD803, the window opener's secured ventilation position (VELUX KLF 200 API
/// specification, section 14.2).
static constexpr DeviceFeatures VENT = 1U << 4U;
/// Named position 0xD809, sent by the Partial Position button: a garage opener runs its stored
/// partial position (issue #163).
static constexpr DeviceFeatures PARTIAL = 1U << 5U;
/// Named position 0xD807, sent by the Pedestrian Position button.
/// \todo Confirm on a real gate opener that 0xD807 opens it to a pedestrian position (experimental).
static constexpr DeviceFeatures PEDESTRIAN = 1U << 6U;
/// The kind may take the 8-byte position and force-open EXECUTE with functional parameter 1 set, which
/// is sent only when a travel speed (`silent` or `fast`) is chosen; otherwise, and for a kind without
/// the bit, they use the 6-byte main-parameter form. On covers functional parameter 1 is the travel
/// speed (VELUX KLF 200 API specification, Appendix 2), so the `travel_speed:` and `silent:` keys need
/// this bit; that proxy holds only for covers, which are the only kinds with those keys.
static constexpr DeviceFeatures EXTENDED_POSITION_FRAME = 1U << 7U;
/// Heater window sensor and open-window-detection switch.
static constexpr DeviceFeatures WINDOW_COMPANIONS = 1U << 8U;
/// Direction inverted by default (horizontal awnings: IO 0 is closed).
static constexpr DeviceFeatures INVERTED = 1U << 9U;
/// Read and commanded as a domestic hot water heater: family-0x00 registers (proto_dhw.h) for its
/// readings and mode, the status main parameter for its target temperature. Gates the water heater
/// poll, its register reads and writes, and the `water_heater:` platform.
static constexpr DeviceFeatures WATER_HEATER = 1U << 10U;
/// A night light built into a heater with private registers: on/off in register 0x0144, brightness in
/// 0x0142 (proto_heating.h). Gates the heater's `night_light:` light entity.
static constexpr DeviceFeatures NIGHT_LIGHT = 1U << 11U;
/// The temperature range the device encodes its temperature over is not fixed, so the YAML
/// `min_temperature:` / `max_temperature:` apply. A device with a fixed range (a heat pump) lacks it.
/// Set exactly on the kinds whose heating profile takes a temperature without `fixed_range`
/// (heating_profiles_test.cpp).
static constexpr DeviceFeatures TEMPERATURE_RANGE = 1U << 12U;
/// An energy meter in a heater with private registers: the energy consumed, in whole kWh, in register
/// 0x0116, and the rated power and voltage in 0x011A (proto_heating.h). Gates the heater's `energy:`,
/// `power:`, `rated_power:` and `rated_voltage:` sensors, each of which opts the device into the reads it
/// shows.
static constexpr DeviceFeatures ENERGY_METER = 1U << 13U;
}  // namespace device_feature

/// @brief True when every bit of `bit` is set in `set`.
/// @param set A feature word.
/// @param bit One bit, or several that must all be set.
constexpr bool has_feature(DeviceFeatures set, DeviceFeatures bit) { return (set & bit) == bit; }

}  // namespace home_io_control
}  // namespace esphome
