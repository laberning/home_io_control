#pragma once

/// @file platform_lr1121_firmware_update_button.h
/// @brief Hub-level button that triggers the LR1121 transceiver-firmware flash sequence.
/// @ingroup hioc_platforms
///
/// Like IOHomeAcceptForeignPairingSwitch, this entity is hub-level, not device-bound — it has no
/// `io_device_id` and isn't declared through a separate platform entry. It is created dynamically
/// from the presence of `home_io_control.lr1121_firmware_update:` (see `__init__.py`'s
/// `_create_lr1121_firmware_update()`), bound directly to the hub instance being built. See
/// hub_lr1121_firmware_update.cpp for what a press actually does — every rejection path is a
/// cached-verdict log, not a fresh bootloader entry. Once a press does proceed, ADR 0020's
/// invariant applies: every exit after a bootloader excursion reboots the ESP32.

// IOHOME_LR1121_FIRMWARE_UPDATE is only visible after something pulls in esphome/core/defines.h
// (via hub_core.h's own #include "esphome/core/hal.h") — these #includes must run before the
// #ifdef check, not after (see radio_lr1121_firmware_updater.h for the fuller explanation of why
// this ordering matters and how it stayed invisible in host unit tests).
#include "esphome/components/button/button.h"
#include "hub_core.h"
#include "platform_entity_base.h"

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE

namespace esphome {
namespace home_io_control {

/// @brief Hub-level button entity: press dispatches to
/// IOHomeControlComponent::trigger_lr1121_firmware_update().
/// @ingroup hioc_platforms
class IOHomeLr1121FirmwareUpdateButton : public button::Button, public Component, public HubBoundEntity {
 public:
  void dump_config() override {}

 protected:
  /// @brief When pressed, hand off to the hub — see hub_lr1121_firmware_update.cpp for the full
  /// guard/decision/flash sequence.
  void press_action() override { this->parent_->trigger_lr1121_firmware_update(); }
};

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_FIRMWARE_UPDATE
