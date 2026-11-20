#pragma once

/// @file proto_device_features.h
/// @brief Capabilities of a device kind: what a (node type, subtype) can do.
/// @ingroup hioc_protocol
///
/// A capability is a name (DeviceFeature); a device kind's capabilities are a DeviceFeatures set of
/// those names (ADR 0098). The sets are carried by the rows of the node-type table
/// (proto_device_types.h) and resolved onto each device record, so the hub, the platforms and the
/// Python codegen ask one place whether a device has a feature instead of branching on a device type.
/// A capability says *whether* a device can do something; how it is commanded stays in the command
/// builders and the profile tables.
///
/// This leaf header includes only the standard headers `<cstdint>` and `<limits>`: IoDevice (proto_device_model.h)
/// carries a DeviceFeatures set, and so does every row of the table in proto_device_types.h.

#include <cstdint>
#include <limits>

namespace esphome {
namespace home_io_control {

/// @brief One capability of a device kind.
/// @details Adding a capability is one more enumerator before COUNT, one mirror entry in
/// hub_validators.py (`FEATURES`) and the rows that have it; nobody chooses a bit position.
/// device_features_sync_test.cpp checks that the Python mirror names the same capabilities row by row.
/// The experimental status of a kind is not a capability: it is the row flag
/// device_type_flags::EXPERIMENTAL.
enum class DeviceFeature : uint8_t {
  /// EXECUTE carries no functional parameter at all (VELUX KLF 200 API specification, Appendix 2:
  /// "main parameter only"). Read by main_parameter_only_variant() and the table invariants; the frame
  /// shape itself is EXTENDED_POSITION_FRAME's job.
  MAIN_PARAMETER_ONLY,
  /// Intermediate main-parameter values: a percent position, a brightness or a heat level.
  LEVELS,
  /// Slat angle (functional parameter 3) through the extended status and the tilt EXECUTE.
  TILT,
  /// Named position 0xD800, the favourite ("my") position. Absent where a device has refused the alias
  /// (garage openers, issue #163).
  FAVORITE,
  /// Named position 0xD803, the window opener's secured ventilation position (VELUX KLF 200 API
  /// specification, section 14.2).
  VENT,
  /// Named position 0xD809, sent by the Partial Position button: a garage opener runs its stored
  /// partial position (issue #163).
  PARTIAL,
  /// Named position 0xD807, sent by the Pedestrian Position button.
  /// \todo Confirm on a real gate opener that 0xD807 opens it to a pedestrian position (experimental).
  PEDESTRIAN,
  /// The kind may take the 8-byte position and force-open EXECUTE with functional parameter 1 set, which
  /// is sent only when a travel speed (`silent` or `fast`) is chosen; otherwise, and for a kind without
  /// the capability, they use the 6-byte main-parameter form. On covers functional parameter 1 is the travel
  /// speed (VELUX KLF 200 API specification, Appendix 2), so the `travel_speed:` and `silent:` keys need
  /// this capability; that proxy holds only for covers, which are the only kinds with those keys.
  EXTENDED_POSITION_FRAME,
  /// Heater window sensor and open-window-detection switch.
  WINDOW_COMPANIONS,
  /// Direction inverted by default (horizontal awnings: IO 0 is closed).
  INVERTED,
  /// Read and commanded as a domestic hot water heater: family-0x00 registers (proto_heating.h) for its
  /// readings and mode, the status main parameter for its target temperature (proto_dhw.h). Gates the water heater
  /// poll, its register reads and writes, and the `water_heater:` platform.
  WATER_HEATER,
  /// A night light built into a heater with private registers: on/off in register 0x0144, brightness in
  /// 0x0142 (proto_heating.h). Gates the heater's `night_light:` light entity.
  NIGHT_LIGHT,
  /// The temperature range the device encodes its temperature over is not fixed, so the YAML
  /// `min_temperature:` / `max_temperature:` apply. A device with a fixed range (a heat pump) lacks it.
  /// Set exactly on the kinds whose heating profile takes a temperature without `fixed_range`
  /// (heating_profiles_test.cpp).
  TEMPERATURE_RANGE,
  /// A mixed towel dryer (warmed by the house's water loop, with an electric element as well) reports in
  /// register 0x0153 whether its central-heating mode is on (1) or the element is in use (0)
  /// (proto_heating.h). Gates the climate entry's `central_heating_mode:` binary sensor.
  /// \todo Experimental: read on one Atlantic Adelis mixed unit (issue #243), confirm on a second one.
  CENTRAL_HEATING_MODE,
  /// A heater that detects whether someone is in the room and lowers its temperature while the room is
  /// empty: register 0x0101 switches the detection on (1) or off (0) and register 0x0110 reports whether it
  /// sees someone (proto_heating.h). Gates the climate entry's `occupancy_detection:` switch and
  /// `occupancy:` binary sensor.
  /// \todo Experimental except on the Sauter Hekla (issue #222): confirm on an AGILIA IO and an EMOTION 3 H
  ///       that they answer both registers.
  OCCUPANCY,
  /// A heater that counts the energy it has used, in whole kilowatt-hours, in register 0x0116
  /// (proto_heating.h). Gates the climate entry's `energy:` sensor.
  /// \todo Experimental except on the Sauter Tahual (PR #238): compare with the display on an EMOTION 3 H and an
  ///       Adelis, which answer the register, and check that a Hekla answers it.
  ENERGY_METER,
  /// A towel dryer whose boost and drying runs the hub can show: the minutes left of the running run
  /// (registers 0x013A and 0x013C) and the durations set for them (0x013B and 0x013D, proto_heating.h). Gates the
  /// climate entry's `remaining_time:`, `boost_duration:` and `drying_duration:` sensors.
  /// \todo Experimental: read on Atlantic Adelis units (issue #202); confirm on a second model and check
  ///       that a dryer without the registers answers them empty.
  RUN_TIMERS,
  /// A heater whose status reply reports the heating level it applies now (comfort, eco, frost protection, ...):
  /// a plain heater, which is commanded with levels, and a towel dryer, whose programme the hub follows through the
  /// level. Set exactly on the kinds whose heating profile reports a level (device_types_table_test.cpp). Gates the
  /// climate entry's `heating_level:` text sensor; the hub reads no level from a radiator or a heater with a light,
  /// and a pilot-wire interface's status carries none, so the key is rejected there.
  /// \todo Experimental on a towel dryer: its status read is captured on one unit in comfort, eco and boost, not yet
  ///       during a drying run or a temporary override.
  REPORTS_HEATING_LEVEL,
  /// A device kind that answers no status read: a device that keeps its values in data objects and has no state to
  /// poll for. The hub never arms or queues a status poll for it (the request after boot, a refresh, a poll
  /// after another controller's traffic); only its readings are read, on their own schedule.
  NO_STATUS_READ,
  /// An Atlantic heat pump that keeps its heating and hot-water energy counters, in whole kilowatt-hours, in entries
  /// of its data object (TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP, offsets 0x0023 and 0x0025). Gates the `sensor:`
  /// entry's `heating_energy:` and `hot_water_energy:` keys.
  /// \todo Experimental: read on one Atlantic Alfea Duo Extensa A.I. (issue #244); confirm on a second model.
  HEAT_PUMP_ENERGY,
  /// An Atlantic heat pump that keeps the outdoor temperature its sensor measures, in tenths of a degree, in an entry
  /// of its data object (TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP, offset 0x019B). Gates the `sensor:` entry's
  /// `outdoor_temperature:` key.
  /// \todo Experimental: read on one Atlantic Alfea Duo Extensa A.I. (issue #244) between 11.4 and 14.7 degrees;
  ///       a value below zero has not been seen yet.
  HEAT_PUMP_OUTDOOR_TEMPERATURE,
  COUNT,  ///< Not a capability: the number of capabilities, for the storage check of DeviceFeatures.
};

/// @brief The capabilities under their short names (`device_feature::NIGHT_LIGHT`).
namespace device_feature {
using enum DeviceFeature;
}  // namespace device_feature

/// @brief A set of DeviceFeature capabilities.
/// @details A value type over a fixed-width mask; the mask is an implementation detail. A single
/// DeviceFeature converts implicitly to a one-element set, and `|` builds larger sets, so a row reads
/// `WINDOW_COMPANIONS | NIGHT_LIGHT`.
class DeviceFeatures {
 public:
  /// The empty set.
  constexpr DeviceFeatures() = default;
  /// A set of exactly one capability.
  constexpr DeviceFeatures(DeviceFeature feature) : mask_(mask_of(feature)) {}

  /// @brief True when every capability of `other` is in this set.
  /// @param other One capability, or a set that must be contained entirely.
  [[nodiscard]] constexpr bool contains(DeviceFeatures other) const { return (mask_ & other.mask_) == other.mask_; }
  /// @brief True when the sets have at least one capability in common.
  [[nodiscard]] constexpr bool intersects(DeviceFeatures other) const { return (mask_ & other.mask_) != 0; }
  /// @brief True when the set holds no capability.
  [[nodiscard]] constexpr bool empty() const { return mask_ == 0; }
  /// @brief This set without the capabilities of `other`.
  [[nodiscard]] constexpr DeviceFeatures without(DeviceFeatures other) const {
    return DeviceFeatures{mask_ & ~other.mask_};
  }

  /// Union.
  [[nodiscard]] friend constexpr DeviceFeatures operator|(DeviceFeatures lhs, DeviceFeatures rhs) {
    return DeviceFeatures{lhs.mask_ | rhs.mask_};
  }
  constexpr DeviceFeatures &operator|=(DeviceFeatures rhs) {
    mask_ |= rhs.mask_;
    return *this;
  }
  /// Set equality.
  [[nodiscard]] friend constexpr bool operator==(DeviceFeatures lhs, DeviceFeatures rhs) = default;

 private:
  using Mask = uint64_t;
  explicit constexpr DeviceFeatures(Mask mask) : mask_(mask) {}
  static constexpr Mask mask_of(DeviceFeature feature) { return Mask{1} << static_cast<unsigned>(feature); }

  Mask mask_ = 0;

  static_assert(static_cast<unsigned>(DeviceFeature::COUNT) <= std::numeric_limits<Mask>::digits,
                "every DeviceFeature must fit the storage of DeviceFeatures; widen Mask");
};

/// @brief The set of two capabilities.
constexpr DeviceFeatures operator|(DeviceFeature lhs, DeviceFeature rhs) {
  return DeviceFeatures{lhs} | DeviceFeatures{rhs};
}

/// @brief True when `set` holds every capability of `feature`.
/// @param set A capability set.
/// @param feature One capability, or a set that must be entirely contained.
constexpr bool has_feature(DeviceFeatures set, DeviceFeatures feature) { return set.contains(feature); }

}  // namespace home_io_control
}  // namespace esphome
