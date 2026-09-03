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
/// hub_oneway_key_adoption.cpp) produces an identity whose key is *not* the hub's own, and it
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
  /// (`resolve_oneway_wire_profile()`). `UNKNOWN` entries are skipped when the sweep runs, so a
  /// one- or two-class override is expressed by leaving the rest `UNKNOWN`. Ignored by the Somfy
  /// enrollment gesture, which always uses `io_device_type`. See ADR 0032.
  std::array<DeviceType, 3> enrollment_classes{DeviceType::UNKNOWN, DeviceType::UNKNOWN, DeviceType::UNKNOWN};

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
/// EXECUTE follow-up — the gesture a real KLI 310/313 PROG press produces (issue #74 capture +
/// `samr037/iohc-flipper` `tx_runner.c` + the KLI manual). See ADR 0032.
enum class EnrollGesture : uint8_t { SOMFY, VELUX_KLI };

/// @brief Vendor-divergent 1W wire settings for a controller identity.
///
/// Phase 1 (ADR 0031) is the EXECUTE ACEI; Phase 2 (ADR 0032) adds the enrollment gesture and the
/// class sweep — see analysis/oneway_vendor_wire_profile_plan.md.
struct OneWayWireProfile {
  uint8_t execute_acei;          ///< payload[1] of a 1W CMD_EXECUTE (0x00) frame.
  bool profile_is_a_guess;       ///< true when `manufacturer` matched no known 1W wire profile.
  EnrollGesture enroll_gesture;  ///< Which enrollment gesture this manufacturer's actuators expect.
  /// Device classes a VELUX_KLI `0x30` sweep targets, `UNKNOWN` entries skipped. All-`UNKNOWN` for
  /// SOMFY, whose `0x30` goes to the identity's own `io_device_type` instead.
  std::array<DeviceType, 3> enrollment_classes;
};

/// The three device classes a real VELUX KLI PROG gesture sweeps its `0x30` across — roller
/// shutter, awning, dual shutter — and never any other (issue #74 capture, decoded with
/// `broadcast_target_type()`; matches `samr037/iohc-flipper`'s `PAIR_DST_{WINDOW,SHUTTER,OTHER}`).
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
