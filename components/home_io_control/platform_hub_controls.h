#pragma once

/// @file platform_hub_controls.h
/// @brief Hub-level control entities: the discovery button, the scan-paired-devices button, the
/// two arming switches, and the pairing-result diagnostic text sensor.
/// @ingroup hioc_platforms
///
/// These act on the hub as a whole and have no device to bind to (HubBoundEntity), and are
/// created from the `home_io_control:` block or a dedicated `button:` entry rather than a
/// device-bound platform entry — for the shared reason, see HubBoundEntity in
/// platform_entity_base.h. They are created dynamically the same way `tuning: {ui_controls: true}`
/// creates its number/select entities, and `set_parent()` is called with the very hub instance
/// being built.

#include <functional>
#include <utility>

#include "esphome/components/button/button.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Shared body for the hub-level arming switches.
///
/// Both arming switches (2W key extraction, 1W controller-key adoption) have the same three
/// behaviours: forward the toggle to the hub, publish the resulting state, and mirror the hub's
/// own disarm events (auto-off timeout, or a successful recovery) so the entity never shows
/// "on" for a window that already closed. Only the two hub calls differ, so those are the two
/// hooks; everything else lives here once.
///
/// Deliberately NOT one concrete class parameterized by an enum or a std::function: the two
/// switches arm independent security-sensitive listeners, and keeping them distinct C++ types
/// means "wire the wrong one" is a compile error rather than a codegen bug.
/// @ingroup hioc_platforms
class HubArmingSwitch : public switch_::Switch, public Component, public HubBoundEntity {
 public:
  /// @brief Register the armed-state callback so this entity mirrors the hub's own disarm events.
  /// No-op when no parent is wired (codegen always wires one before setup() runs).
  void setup() final;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const final { return setup_priority::DATA; }

 protected:
  /// @brief Forward the toggle to the hub and publish the state. Publishing still happens with no
  /// parent wired, so an unwired switch reads as off rather than unknown in Home Assistant.
  void write_state(bool state) final;

  /// @brief Arm or disarm this switch's listener on the hub.
  virtual void arm(bool state) = 0;
  /// @brief Subscribe @p callback to the hub's armed-state changes for this listener.
  virtual void subscribe_armed(std::function<void(bool)> callback) = 0;
};

/// @brief Hub-level switch entity: ON arms the key-extraction responder for 10 minutes, OFF
/// disarms it immediately. Publishes its own state changes when the hub disarms itself
/// (successful extraction or auto-off timeout), not just on a user-initiated toggle.
///
/// Created dynamically from `home_io_control.accept_foreign_pairing: true` (see `__init__.py`'s
/// `_create_hub_arming_switch()`), not through a `switch:` platform entry — see the file header
/// for why. See key_extraction_responder.cpp for what arming actually does.
///
/// @note Hardware-confirmed on real RF hardware, but not yet against a third-party hub — see
/// key_extraction_responder.cpp.
/// @ingroup hioc_platforms
class IOHomeAcceptForeignPairingSwitch : public HubArmingSwitch {
 protected:
  void arm(bool state) override { this->parent_->set_key_extraction_armed(state); }
  void subscribe_armed(std::function<void(bool)> callback) override {
    this->parent_->set_key_extraction_armed_callback(std::move(callback));
  }

  /// @brief Dump configuration to the log.
  void dump_config() override;
};

/// @brief Hub-level switch entity: ON arms the 1W key-recovery listener, OFF disarms it
/// immediately. Publishes its own state changes when the hub disarms itself (after a key is
/// recovered, or on auto-off timeout), not just on a user-initiated toggle.
///
/// The one-way sibling of IOHomeAcceptForeignPairingSwitch: same hub-level, non-device-bound
/// shape, created dynamically from `home_io_control.recover_oneway_key: true`. See
/// oneway_key_adoption.cpp for what arming actually does.
///
/// The two features are deliberately independent: 2W key extraction impersonates an unpaired
/// device so a foreign hub pairs *to* us, while this one only listens for a key a 1W device
/// broadcasts of its own accord. Arming one never arms the other.
/// @ingroup hioc_platforms
class IOHomeRecoverOneWayKeySwitch : public HubArmingSwitch {
 protected:
  void arm(bool state) override { this->parent_->set_oneway_key_adoption_armed(state); }
  void subscribe_armed(std::function<void(bool)> callback) override {
    this->parent_->set_oneway_key_adoption_armed_callback(std::move(callback));
  }

  /// @brief Dump configuration to the log.
  void dump_config() override;
};

/// @brief Button entity that triggers device discovery and pairing when pressed in Home Assistant.
/// @ingroup hioc_platforms
class IOHomeDiscoverButton : public button::Button, public Component, public HubBoundEntity {
 public:
  void dump_config() override {}

 protected:
  /// @brief When button is pressed, queue a discovery/pair operation.
  void press_action() override { this->parent_->queue_discover_and_pair(); }
};

/// @brief Button entity that runs the `scan_paired_devices` roll-call when pressed.
///
/// The same operation as the native API action of that name, which is unchanged and still
/// registered — this is an additional trigger path, not a replacement. It exists because the
/// roll-call is the fastest bring-up route for the common case of a user who already holds a
/// system key extracted from a hub that has paired all its devices: every device that trusts the
/// key answers with a ready-to-paste YAML snippet, with no pairing handshake at all. Reaching that
/// through Developer Tools was a speed bump on a one-shot step.
///
/// Output stays where the action puts it: the multi-line log and the
/// `esphome.home_io_control_action_result` event. Deliberately no companion result sensor — unlike
/// the pairing button's "Last Pairing Result", the roll-call report is a multi-device document,
/// not a state.
///
/// Created only when `home_io_control.scan_paired_devices_button: true` (see `__init__.py`'s
/// `_create_scan_paired_devices_button()`), unlike IOHomeDiscoverButton which is an unconditional
/// `button:` platform entry — see the file header for why hub entities come from the hub block.
/// @ingroup hioc_platforms
class IOHomeScanPairedDevicesButton : public button::Button, public Component, public HubBoundEntity {
 public:
  void dump_config() override {}

 protected:
  /// @brief When pressed, run the roll-call — see
  /// IOHomeControlComponent::trigger_scan_paired_devices() for the busy-guard rationale.
  void press_action() override { this->parent_->trigger_scan_paired_devices(); }
};

/// @brief Diagnostic text sensor that publishes PairingTelemetry::result_sensor_string()
/// after every pairing attempt.
///
/// The published string is the frozen `v1;...` format documented on
/// PairingTelemetry::result_sensor_string() — the Phase 2 automated-rig read-back contract.
/// Nothing is published before the first pairing attempt of this boot.
/// @ingroup hioc_platforms
class IOHomePairingResultTextSensor : public text_sensor::TextSensor, public Component, public HubBoundEntity {
 public:
  /// @brief Register the pairing-result callback.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Publish the latest pairing telemetry result string.
  void on_pairing_result_();
};

}  // namespace home_io_control
}  // namespace esphome
