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
/// @note Ownership. These identities ultimately belong to the `OneWayTransmitter` collaborator
/// (ADR 0004), held by value in the hub, which keeps only the wiring. That collaborator does not
/// exist yet — it arrives with the transmit engine. Until then the collection lives here and the
/// hub holds it; **move ownership into `OneWayTransmitter` when it lands** rather than leaving a
/// second long-lived container on the component.

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
  /// what a 1W entity binds to. The class occupies bits [9:2] of bytes 1-2, so it **spans two
  /// bytes**: the light class (0x06) encodes to `00 01 BF`, not `00 00 BF`. This is the exact
  /// inverse of broadcast_target_type() (proto_codecs.cpp) and shares its shift constants so the
  /// two cannot drift apart. The low six bits are the subtype field with every bit set — i.e.
  /// "any subtype of this class", which is what makes the address a broadcast.
  /// @param out Output: 3-byte destination address.
  void broadcast_address(uint8_t out[NODE_ID_SIZE]) const {
    const auto type_raw = static_cast<uint16_t>(this->io_device_type);
    out[0] = 0;
    out[1] = static_cast<uint8_t>(type_raw >> DEVICE_TYPE_LOW_BITS_SHIFT);
    out[2] = static_cast<uint8_t>((type_raw << DEVICE_TYPE_HIGH_BITS_SHIFT) | DEVICE_SUBTYPE_MASK);
  }
};

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
