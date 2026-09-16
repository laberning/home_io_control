#pragma once

/// @file oneway_controller.h
/// @brief Controller identities for the one-way (1W) protocol.
/// @ingroup hioc_protocol
///
/// 1W frames are **class-addressed**: a command goes to a typed broadcast address
/// `(io_device_type << 6) | 0x3F`, not to an individual device. Nothing on the wire names a
/// device, so a 1W entity has no node address to bind to. What distinguishes one 1W control
/// surface from another is the *controller* doing the transmitting — its source address, its
/// network key, and the device class it speaks to. That triple is a controller identity, and it
/// takes the place node addressing has for 2W. See ADR 0027.
///
/// A hub holds several, deliberately: adopting a foreign 1W network's key (see
/// oneway_key_adoption.cpp) produces an identity whose key is *not* the hub's own, and it
/// must coexist with identities on the hub's own network rather than replace them.
///
/// @note Ownership. These identities belong to the `OneWayTransmitter` collaborator
/// (oneway_transmitter.h), held by value in the hub, which keeps only the wiring (ADR 0004).
/// This header owns the types; it does not own an instance of them.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_sizes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

/// @brief Which preamble/CTRL1 shape a 1W identity's bursts use (`low_power:` on the identity).
///
/// **Tri-state, unlike the per-device 2W `low_power` key (ADR 0029), which is a plain bool
/// defaulting to `false`.** Here, *unset* is its own state and is not equivalent to `false`: it
/// keeps every 1W transmit byte- and timing-identical to the hardware-validated Somfy path.
/// `false` opts an identity into the ADR 0029 shape (short preamble, no wake flag); `true` adds a
/// wake-up copy for a duty-cycled receiver. See ADR 0038 for why unset survives as a knowingly
/// non-conforming default rather than being folded into `false`.
enum class OneWayPowerClass : uint8_t {
  LEGACY_LONG,   ///< `low_power:` unset: LONG_PREAMBLE on every copy, CTRL1 0x00 everywhere.
  ALWAYS_ALIVE,  ///< `low_power: false`: normal start preamble on every copy, CTRL1 0x00.
  LOW_POWER,     ///< `low_power: true`: copy 1 LONG_PREAMBLE + CTRL1_LOW_POWER, repeats normal.
};

/// @brief Human-readable name for a OneWayPowerClass, as it appears in the boot log.
/// @param power_class Power class to name.
/// @return Null-terminated name such as "always-alive".
const char *oneway_power_class_name(OneWayPowerClass power_class);

/// @brief Which wake-up shape one copy of a 1W burst gets.
enum class OneWayPreamble : uint8_t {
  WAKE,    ///< The long wake-up preamble (`LONG_PREAMBLE`), paired with `CTRL1_LOW_POWER` set.
  NORMAL,  ///< The runtime-tunable `normal_start_preamble`, CTRL1's low-power bit clear.
};

/// @brief The preamble and CTRL1 shape one copy of a 1W burst gets.
///
/// Chip-neutral by design: this header names bytes, not chips, and does not know about
/// `TuningConfig` — resolving `OneWayPreamble::NORMAL` to an actual byte count is
/// `OneWayTransmitter`'s job (oneway_transmitter.h), the one place in the controller layer that
/// holds a `TuningConfig *`.
struct OneWayCopyShape {
  OneWayPreamble preamble;  ///< Which preamble this copy transmits with.
  bool low_power_flag;      ///< Whether this copy sets `CTRL1_LOW_POWER`.
};

/// @brief Resolve which preamble/CTRL1 shape one copy of a burst gets, from the identity's power
/// class and the copy's position in the burst.
///
/// Pure: no radio, no tuning, testable on its own. Implements the table in ADR 0038 — `LEGACY_LONG`
/// and `ALWAYS_ALIVE` are uniform across every copy; only `LOW_POWER` varies by position, putting
/// the wake-up shape on copy 0 alone — what a real remote's GEAR/EXECUTE burst does, and the
/// mirror of how `ExchangeEngine` (ADR 0029) already picks a 2W start frame's preamble from the
/// target's own low-power declaration.
/// @param power_class The identity's configured power class.
/// @param copy_index 0-based position of this copy within the burst.
/// @return The preamble and CTRL1 shape for that copy.
inline OneWayCopyShape oneway_burst_copy_shape(OneWayPowerClass power_class, uint8_t copy_index) {
  switch (power_class) {
    case OneWayPowerClass::ALWAYS_ALIVE:
      return {OneWayPreamble::NORMAL, /*low_power_flag=*/false};
    case OneWayPowerClass::LOW_POWER:
      return copy_index == 0 ? OneWayCopyShape{OneWayPreamble::WAKE, /*low_power_flag=*/true}
                             : OneWayCopyShape{OneWayPreamble::NORMAL, /*low_power_flag=*/false};
    case OneWayPowerClass::LEGACY_LONG:
    default:
      return {OneWayPreamble::WAKE, /*low_power_flag=*/false};
  }
}

/// @brief One configured 1W controller identity.
///
/// Fixed-size key and address material; the only heap is the `id` handle, which mirrors how
/// device IDs are held elsewhere in this component.
/// @ingroup hioc_protocol
struct OneWayControllerIdentity {
  std::string id;                                  ///< YAML handle entities reference.
  uint8_t node_id[NODE_ID_SIZE]{};                 ///< Source address we transmit as (configured or derived).
  uint8_t system_key[AES_KEY_SIZE]{};              ///< Network key for this identity; may differ per identity.
  uint8_t manufacturer{0};                         ///< Manufacturer ID; unused until the enrollment phase.
  DeviceType io_device_type{DeviceType::UNKNOWN};  ///< Device class this identity commands.
  uint16_t initial_sequence{0};                    ///< Seed for this identity's rolling counter on first use.
  bool node_id_derived{false};                     ///< True when `node_id` was derived rather than configured.
  bool enrollment_with_mac{
      false};  ///< Whether the enroll button's 0x30 carries a MAC trailer (`enrollment_with_mac:`).
  /// EXECUTE ACEI override (`execute_acei:`). 0 = "not overridden, use resolve_oneway_wire_profile()";
  /// any non-zero value wins. 0 is a safe sentinel: ACEI_VALID_BIT is bit 0, so a valid ACEI is
  /// always odd — the schema rejects `execute_acei: 0` so this cannot be reached by mistake.
  uint8_t execute_acei{0};
  /// When true (`execute_broadcast: all`), 1W EXECUTE frames go to the all-devices address
  /// `00 00 3F` regardless of `io_device_type` — what a handheld cover remote of either vendor
  /// does. false = the typed per-class destination (current default). Not a vendor axis; see
  /// ADR 0031.
  bool execute_broadcast_all{false};
  /// Override for the device classes a VELUX enrollment `0x30` sweep targets (`enrollment_classes:`).
  /// All-`UNKNOWN` (the default) means "not set — use the manufacturer profile's list"
  /// (`resolve_oneway_wire_profile()`), which only fits exterior shading; a KLI 312 interior blind
  /// needs `{BLIND, VENETIAN_BLIND}` here. `UNKNOWN` entries are skipped when the sweep runs, so a
  /// one- or two-class override is expressed by leaving the rest `UNKNOWN`. Ignored by the Somfy
  /// enrollment gesture, which always uses `io_device_type`. See ADR 0032.
  std::array<DeviceType, 3> enrollment_classes{DeviceType::UNKNOWN, DeviceType::UNKNOWN, DeviceType::UNKNOWN};
  /// Preamble/CTRL1 shape every burst this identity sends uses (`low_power:`). Applies to every
  /// 1W TX of the identity -- commands, positions, enrollment (both gestures), un-enrollment. Last
  /// field: codegen emits a designated initialiser in declaration order. See ADR 0038.
  OneWayPowerClass power_class{OneWayPowerClass::LEGACY_LONG};

  /// @brief Enrollment / typed-class destination address for this identity.
  ///
  /// 1W addresses a device *class*, never a node. Delegates to encode_broadcast_address()
  /// (proto_codecs.h), the single place the bit layout is documented, so this and
  /// broadcast_target_type() (its decode counterpart) cannot drift apart. Note: an EXECUTE frame
  /// uses the all-devices address instead when `execute_broadcast_all` is set — this helper is the
  /// typed-class destination only.
  /// @param out Output: 3-byte destination address.
  void broadcast_address(uint8_t out[NODE_ID_SIZE]) const { encode_broadcast_address(this->io_device_type, out); }
};

/// @brief Which 1W enrollment gesture a manufacturer's actuators expect.
///
/// SOMFY: one `0x30` add-controller burst to the identity's own `io_device_type` (the shape this
/// project has hardware-validated). VELUX_KLI: a `0x39` clear to the all-devices address, then a
/// `0x30` burst to **each** class in `OneWayWireProfile::enrollment_classes`, then a STOP+DOWN
/// EXECUTE follow-up — the gesture a real KLI remote produces (issue #74 capture +
/// the KLI manual), confirmed on a VELUX SML roller shutter and on KLI 312
/// interior blinds. See ADR 0032.
enum class EnrollGesture : uint8_t { SOMFY, VELUX_KLI };

/// @brief Vendor-divergent 1W wire settings for a controller identity.
///
/// The EXECUTE ACEI is decided per ADR 0031; the enrollment gesture and the class sweep per
/// ADR 0032.
struct OneWayWireProfile {
  uint8_t execute_acei;          ///< payload[1] of a 1W CMD_EXECUTE (0x00) frame.
  bool profile_is_a_guess;       ///< true when `manufacturer` matched no known 1W wire profile.
  EnrollGesture enroll_gesture;  ///< Which enrollment gesture this manufacturer's actuators expect.
  /// Device classes a VELUX_KLI `0x30` sweep targets, `UNKNOWN` entries skipped. All-`UNKNOWN` for
  /// SOMFY, whose `0x30` goes to the identity's own `io_device_type` instead.
  std::array<DeviceType, 3> enrollment_classes;
};

/// The default `0x30` sweep for `manufacturer: velux`: the exterior-shading classes a KLI 310/313
/// names — roller shutter, awning, dual shutter (issue #74 capture, decoded with
/// `broadcast_target_type()`; matches `samr037/iohc-flipper`'s `PAIR_DST_{WINDOW,SHUTTER,OTHER}`).
/// Not universal: a KLI 312 interior blind uses blind + venetian blind, so those identities set
/// `enrollment_classes:` from the classes their remote's own `0x2E` names.
static constexpr std::array<DeviceType, 3> VELUX_KLI_ENROLLMENT_CLASSES{DeviceType::ROLLER_SHUTTER, DeviceType::AWNING,
                                                                        DeviceType::DUAL_SHUTTER};

/// @brief Resolve an identity's 1W wire profile from its manufacturer byte.
///
/// Pure. Somfy (0x02) and unset (0x00) both map to the historical Somfy-shaped default
/// (`ONEWAY_EXECUTE_ACEI`, `EnrollGesture::SOMFY`); only VELUX (0x01) is special so far
/// (`ONEWAY_EXECUTE_ACEI_VELUX`, `EnrollGesture::VELUX_KLI`, the class sweep). Any other
/// explicitly-set manufacturer returns the Somfy default with `profile_is_a_guess=true` so the
/// Python schema can warn (`__init__.py` `_validate_oneway_controllers()` — keep the {somfy,
/// velux} set here in sync with the warning there; there is no automated check).
/// @param manufacturer The identity's manufacturer byte (`MANUFACTURER_*`, or a raw value).
inline OneWayWireProfile resolve_oneway_wire_profile(uint8_t manufacturer) {
  constexpr std::array<DeviceType, 3> none{DeviceType::UNKNOWN, DeviceType::UNKNOWN, DeviceType::UNKNOWN};
  switch (manufacturer) {
    case MANUFACTURER_VELUX:
      return {ONEWAY_EXECUTE_ACEI_VELUX, /*profile_is_a_guess=*/false, EnrollGesture::VELUX_KLI,
              VELUX_KLI_ENROLLMENT_CLASSES};
    case MANUFACTURER_SOMFY:
    case 0x00:
      return {ONEWAY_EXECUTE_ACEI, /*profile_is_a_guess=*/false, EnrollGesture::SOMFY, none};
    default:
      return {ONEWAY_EXECUTE_ACEI, /*profile_is_a_guess=*/true, EnrollGesture::SOMFY, none};
  }
}

/// @brief The device classes this identity's `0x30` enrollment sweep will actually target.
/// @param identity The controller identity.
/// @return `enrollment_classes` when the identity overrode it (any entry non-`UNKNOWN`), else the
///         manufacturer profile's list. `UNKNOWN` entries are skipped by the caller.
inline std::array<DeviceType, 3> effective_enrollment_classes(const OneWayControllerIdentity &identity) {
  const bool overridden = identity.enrollment_classes[0] != DeviceType::UNKNOWN ||
                          identity.enrollment_classes[1] != DeviceType::UNKNOWN ||
                          identity.enrollment_classes[2] != DeviceType::UNKNOWN;
  return overridden ? identity.enrollment_classes
                    : resolve_oneway_wire_profile(identity.manufacturer).enrollment_classes;
}

/// @brief The ACEI byte a given identity will put on air for a 1W EXECUTE frame.
/// @param identity The controller identity.
/// @return `execute_acei` when overridden (non-zero), else the manufacturer's profile default.
inline uint8_t effective_execute_acei(const OneWayControllerIdentity &identity) {
  return identity.execute_acei != 0 ? identity.execute_acei
                                    : resolve_oneway_wire_profile(identity.manufacturer).execute_acei;
}

/// @brief Whether this identity's ACEI comes from an explicit `execute_acei:` rather than the profile.
/// @param identity The controller identity.
/// @return true when `execute_acei:` was set (non-zero) and overrides the manufacturer profile default.
inline bool has_execute_acei_override(const OneWayControllerIdentity &identity) { return identity.execute_acei != 0; }

// === Control surface ===

/// Wire-scale position meaning "fully closed" (0 means fully open). Named here because the two
/// values are what OPEN and CLOSE actually are — see encode_oneway_action().
static constexpr uint8_t ONEWAY_POSITION_FULLY_OPEN = 0;
static constexpr uint8_t ONEWAY_POSITION_FULLY_CLOSED = 100;

/// @brief The command a generated 1W button sends.
///
/// The vocabulary of the `commands:` list on a `oneway_controllers:` entry. Kept separate from
/// CoverCommand because two of these are not commands at all on the wire: OPEN and CLOSE are
/// positions 0 and 100, and only look like named commands to a user.
/// @ingroup hioc_protocol
enum class OneWayButtonAction : uint8_t {
  OPEN,      ///< Position 0 (fully open).
  CLOSE,     ///< Position 100 (fully closed).
  STOP,      ///< CoverCommand::STOP.
  VENT,      ///< CoverCommand::VENT.
  FAVORITE,  ///< CoverCommand::FAVORITE.
};

/// @brief How a OneWayButtonAction reaches the wire.
/// @ingroup hioc_protocol
struct OneWayActionEncoding {
  bool is_position{false};                   ///< True when the action is sent as a numeric position.
  uint8_t position{0};                       ///< Position to send when `is_position`.
  CoverCommand command{CoverCommand::STOP};  ///< Named command to send otherwise.
};

/// @brief Resolve a button action to the call that sends it.
///
/// Pure, so the mapping can be tested without a radio, an entity or a hub. OPEN and CLOSE resolve
/// to positions because that is what they are on the wire — there is no distinct open/close
/// opcode, and treating them as named commands would need a second encoding path for no gain.
/// @param action Button action to encode.
/// @return The position-or-command the transmitter should send.
/// @ingroup hioc_protocol
inline OneWayActionEncoding encode_oneway_action(OneWayButtonAction action) {
  OneWayActionEncoding encoding{};
  switch (action) {
    case OneWayButtonAction::OPEN:
      encoding.is_position = true;
      encoding.position = ONEWAY_POSITION_FULLY_OPEN;
      break;
    case OneWayButtonAction::CLOSE:
      encoding.is_position = true;
      encoding.position = ONEWAY_POSITION_FULLY_CLOSED;
      break;
    case OneWayButtonAction::VENT:
      encoding.command = CoverCommand::VENT;
      break;
    case OneWayButtonAction::FAVORITE:
      encoding.command = CoverCommand::FAVORITE;
      break;
    case OneWayButtonAction::STOP:
    default:
      encoding.command = CoverCommand::STOP;
      break;
  }
  return encoding;
}

/// @brief Human-readable name for a button action, as it appears in the diagnostic sensor.
/// @param action Button action to name.
/// @return Null-terminated name such as "OPEN".
/// @ingroup hioc_protocol
const char *oneway_button_action_name(OneWayButtonAction action);

/// @brief The configured 1W controller identities, in YAML declaration order.
///
/// Lookup is by `id` and linear: a hub has a handful of identities, not hundreds, and keeping
/// insertion order makes boot logging read the same as the YAML that produced it.
/// @ingroup hioc_protocol
class OneWayControllerRegistry {
 public:
  /// @brief Add a configured identity. Called from generated code at setup.
  /// @param identity Fully-resolved identity (key inherited or explicit, address configured or derived).
  void add(const OneWayControllerIdentity &identity) { this->identities_.push_back(identity); }

  /// @brief Look up an identity by its YAML handle.
  /// @param id Handle to find.
  /// @return Pointer to the identity, or nullptr if no such handle is configured.
  [[nodiscard]] const OneWayControllerIdentity *get(const std::string &id) const {
    for (const auto &identity : this->identities_) {
      if (identity.id == id)
        return &identity;
    }
    return nullptr;
  }

  /// @brief All configured identities, in declaration order.
  [[nodiscard]] const std::vector<OneWayControllerIdentity> &all() const { return this->identities_; }

  /// @brief Whether any identity is configured.
  [[nodiscard]] bool empty() const { return this->identities_.empty(); }

 private:
  std::vector<OneWayControllerIdentity> identities_;
};

}  // namespace home_io_control
}  // namespace esphome
