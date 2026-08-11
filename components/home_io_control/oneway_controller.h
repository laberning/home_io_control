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
/// takes the place node addressing has for 2W. See ADR 0024.
///
/// A hub holds several, deliberately: adopting a foreign 1W network's key (see
/// hub_oneway_key_adoption.cpp) produces an identity whose key is *not* the hub's own, and it
/// must coexist with identities on the hub's own network rather than replace them.
///
/// @note Ownership. These identities belong to the `OneWayTransmitter` collaborator
/// (oneway_transmitter.h), held by value in the hub, which keeps only the wiring (ADR 0004).
/// This header owns the types; it does not own an instance of them.

#include "proto_codecs.h"
#include "proto_device_model.h"
#include "proto_sizes.h"

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

  /// @brief Typed-broadcast destination address this identity transmits to.
  ///
  /// 1W addresses a device *class*, never a node — which is why the identity, not a device ID, is
  /// what a 1W entity binds to. Delegates to encode_broadcast_address() (proto_codecs.h), the
  /// single place the bit layout is documented, so this and broadcast_target_type() (its decode
  /// counterpart) cannot drift apart.
  /// @param out Output: 3-byte destination address.
  void broadcast_address(uint8_t out[NODE_ID_SIZE]) const { encode_broadcast_address(this->io_device_type, out); }
};

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
