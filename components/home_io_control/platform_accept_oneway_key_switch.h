#pragma once

/// @file platform_accept_oneway_key_switch.h
/// @brief Hub-level switch that arms/disarms the 1W controller-key adoption listener.
/// @ingroup hioc_platforms
///
/// The one-way sibling of IOHomeAcceptForeignPairingSwitch
/// (platform_accept_foreign_pairing_switch.h): same hub-level, non-device-bound shape, created
/// dynamically from `home_io_control.accept_oneway_key: true` (see `__init__.py`'s
/// `_create_accept_oneway_key_switch()`) rather than through a `switch:` platform entry, and
/// bound directly to the hub instance being built. See hub_oneway_key_adoption.cpp for what
/// arming actually does.
///
/// The two features are deliberately independent: 2W key extraction impersonates an unpaired
/// device so a foreign hub pairs *to* us, while this one only listens for a key a 1W device
/// broadcasts of its own accord. Arming one never arms the other.

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Hub-level switch entity: ON arms the 1W key-adoption listener, OFF disarms it
/// immediately. Publishes its own state changes when the hub disarms itself (after a key is
/// adopted, or on auto-off timeout), not just on a user-initiated toggle.
/// @ingroup hioc_platforms
class IOHomeAcceptOneWayKeySwitch : public switch_::Switch, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }

  /// @brief Register the armed-state callback so this entity mirrors the hub's own disarm events.
  void setup() override;

  /// @brief Dump configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Arm or disarm the hub's 1W key-adoption listener.
  /// @param state Desired switch state.
  void write_state(bool state) override;

  IOHomeControlComponent *parent_{nullptr};
};

}  // namespace home_io_control
}  // namespace esphome
