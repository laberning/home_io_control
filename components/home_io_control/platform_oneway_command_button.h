#pragma once

/// @file platform_oneway_command_button.h
/// @brief Generated 1W command button, one per entry in an identity's `commands:` list.
/// @ingroup hioc_platforms
///
/// Created from the hub's `oneway_controllers:` block, never declared as a `button:` platform
/// entry — the same structural choice the key-extraction switch is built on. A user-declared
/// platform entry would have to dispatch on the presence of some key (`commands:`, `enroll:`) to
/// decide what the button *is*, which is exactly how a device-bound switch that merely forgot its
/// `io_device_id` once became the security-sensitive one instead of failing validation. Creating
/// these from the hub block makes that class of mistake impossible: there is no shared schema for
/// two kinds of button to be confused under.
///
/// A press is fire-and-forget. Nothing replies, so a press that a device ignores looks exactly
/// like one it obeyed — the "Last 1W Command" text sensor
/// (platform_oneway_last_command_text_sensor.h) reports what was transmitted, which is the only
/// half of that the hub can know.

#include <string>

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "oneway_controller.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Button entity that sends one 1W command as a configured controller identity.
/// @ingroup hioc_platforms
class IOHomeOneWayCommandButton : public button::Button, public Component, public HubBoundEntity {
 public:
  /// @brief Set the controller identity this button transmits as.
  /// @param id Handle from the `oneway_controllers:` block.
  void set_controller_id(const std::string &id) { this->controller_id_ = id; }

  /// @brief Set which command a press sends.
  /// @param action Button action (see encode_oneway_action()).
  void set_action(OneWayButtonAction action) { this->action_ = action; }

  void setup() override {}
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Queue the command. It goes through the operation queue like all radio work, so it
  /// cannot interleave with a 2W exchange (ADR 0013).
  void press_action() override { this->parent_->send_oneway_action(this->controller_id_, this->action_); }

  std::string controller_id_;
  OneWayButtonAction action_{OneWayButtonAction::STOP};
};

}  // namespace home_io_control
}  // namespace esphome
