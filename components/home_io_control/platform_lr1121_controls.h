#pragma once

/// @file platform_lr1121_controls.h
/// @brief Hub-level LR1121 firmware-update entities: the transceiver-flash button and the
/// bootloader-rewrite arming switch.
/// @ingroup hioc_platforms
///
/// Both are hub-level, not device-bound — there is no `io_device_id` to bind to, they target the
/// hub's own radio (created from the `home_io_control:` block for the shared reason, see
/// HubBoundEntity in platform_entity_base.h). They are created dynamically from the presence of
/// `home_io_control.lr1121_firmware_update:` (the button, behind IOHOME_LR1121_FIRMWARE_UPDATE)
/// and its `bootloader:` sub-block (the switch, behind IOHOME_LR1121_BOOTLOADER_UPDATE); see
/// `__init__.py`'s `_create_lr1121_firmware_update()` / `_create_lr1121_bootloader_update()`.
/// The bootloader define is only ever added from inside the firmware-update path, so it implies
/// IOHOME_LR1121_FIRMWARE_UPDATE and the two guards nest.

// IOHOME_LR1121_FIRMWARE_UPDATE / IOHOME_LR1121_BOOTLOADER_UPDATE are only visible after
// something pulls in esphome/core/defines.h (via hub_core.h's own #include "esphome/core/hal.h")
// — these #includes must run before the #ifdef check, not after (see
// radio_lr1121_firmware_updater.h for the fuller explanation of why this ordering matters and how
// it stayed invisible in host unit tests).
#include "esphome/components/button/button.h"
#include "esphome/components/switch/switch.h"
#include "hub_core.h"
#include "platform_entity_base.h"

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE

namespace esphome {
namespace home_io_control {

/// @brief Hub-level button entity: press dispatches to
/// IOHomeControlComponent::trigger_lr1121_firmware_update().
///
/// See lr1121_firmware_update_controller.cpp for what a press actually does — every rejection path
/// is a cached-verdict log, not a fresh bootloader entry. Once a press does proceed, ADR 0020's
/// invariant applies: every exit after a bootloader excursion reboots the ESP32.
/// @ingroup hioc_platforms
class IOHomeLr1121FirmwareUpdateButton : public button::Button, public Component, public HubBoundEntity {
 public:
  void dump_config() override {}

 protected:
  /// @brief When pressed, hand off to the hub — see lr1121_firmware_update_controller.cpp for the
  /// full guard/decision/flash sequence.
  void press_action() override { this->parent_->trigger_lr1121_firmware_update(); }
};

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE

/// @brief Hub-level switch entity: ON permits the next flash-button press to run the three-stage
/// bootloader-rewrite sequence (when the cached verdict is AVAILABLE); OFF (the default, and the
/// restore-on-boot state -- `ALWAYS_OFF`) refuses it. No auto-off timer: see ADR 0021 for why the
/// window is already self-limiting.
///
/// This is a *permission*, not the two-press "armed" confirmation used elsewhere in this
/// component (`lr1121_flash_confirmation_armed_`) -- see ADR 0021 for why an arming switch
/// (visible in Home Assistant, so "is this armed?" is answerable by looking) was chosen over
/// another invisible two-press window for the one operation in this component with no undo. It
/// can only convert a cached `BootloaderUpgradePath::AVAILABLE` verdict into "run the three-stage
/// sequence"; it never affects any other verdict or guard (see
/// IOHomeControlComponent::set_bootloader_rewrite_allowed()).
/// @ingroup hioc_platforms
class IOHomeLr1121BootloaderRewriteSwitch : public switch_::Switch, public Component, public HubBoundEntity {
 public:
  /// @brief Publish the initial OFF state.
  ///
  /// Unlike the arming switches, this switch has no auto-off timer, so the hub never changes the
  /// permission on its own and there is no state to push back -- but an entity that has never
  /// published reads as *unknown* in Home Assistant, and a safety control whose whole
  /// justification is "is this armed? answerable by looking" (ADR 0021) must not render blank.
  /// Publishing false also makes the configured ALWAYS_OFF restore behaviour observable rather
  /// than merely implied. Publishing does not call write_state(), so this never touches the hub.
  void setup() override;

  /// @brief Dump configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Forward the toggle to the hub as a permission -- see
  /// IOHomeControlComponent::set_bootloader_rewrite_allowed().
  /// @param state Desired switch state.
  void write_state(bool state) override;
};

#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_FIRMWARE_UPDATE
