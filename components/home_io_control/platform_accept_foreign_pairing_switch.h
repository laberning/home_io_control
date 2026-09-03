#pragma once

/// @file platform_accept_foreign_pairing_switch.h
/// @brief Hub-level switch that arms/disarms the "Accept Foreign Pairing" (key-extraction)
/// responder.
/// @ingroup hioc_platforms
///
/// Unlike IOHomeSwitch (platform_switch.h), this entity is hub-level, not device-bound — it has
/// no `io_device_id` and isn't declared through a `switch:` platform entry at all. It is created
/// dynamically from `home_io_control.accept_foreign_pairing: true` (see `__init__.py`'s
/// `_create_accept_foreign_pairing_switch()`), the same way `tuning: {ui_controls: true}`
/// dynamically creates its number/select entities, and bound directly to the hub instance being
/// built (`set_parent()` called with that same object). See key_extraction_responder.cpp for what
/// arming actually does.
///
/// @note Hardware-confirmed on real RF hardware, but not yet against a third-party hub — see
/// key_extraction_responder.cpp.

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Hub-level switch entity: ON arms the key-extraction responder for 10 minutes, OFF
/// disarms it immediately. Publishes its own state changes when the hub disarms itself
/// (successful extraction or auto-off timeout), not just on a user-initiated toggle.
/// @ingroup hioc_platforms
class IOHomeAcceptForeignPairingSwitch : public switch_::Switch, public Component, public HubBoundEntity {
 public:
  /// @brief Register the armed-state callback so this entity mirrors the hub's own disarm events.
  void setup() override;

  /// @brief Dump configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Arm or disarm the hub's key-extraction responder.
  /// @param state Desired switch state.
  void write_state(bool state) override;
};

}  // namespace home_io_control
}  // namespace esphome
