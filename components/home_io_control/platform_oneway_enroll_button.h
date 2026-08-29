#pragma once

/// @file platform_oneway_enroll_button.h
/// @brief Generated 1W enrollment button, one per `oneway_controllers:` identity with
/// `enrollment: true` set.
/// @ingroup hioc_platforms
///
/// Created from the hub's `oneway_controllers:` block, never declared as a `button:` platform
/// entry — same structural reason as platform_oneway_command_button.h: a user-declared entry
/// would have to dispatch on the presence of the `enrollment:` key to decide what the button is,
/// which is exactly how a device-bound switch that merely forgot a key once became the
/// security-sensitive one instead of failing validation.
///
/// A press sends `0x39` then `0x30`, back to back (OneWayTransmitter::send_enrollment()) — the
/// documented 1W pairing handshake. The receiver's half of enrollment is a physical 2s PROG hold
/// on the actuator itself; that physical requirement is the real interlock against a stray or
/// unintended enrollment, not a software arming switch — see ADR 0026.

#include <string>

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Button entity that registers a configured controller identity as a 1W controller.
/// @ingroup hioc_platforms
class IOHomeOneWayEnrollButton : public button::Button, public Component, public HubBoundEntity {
 public:
  /// @brief Set the controller identity this button enrolls.
  /// @param id Handle from the `oneway_controllers:` block.
  void set_controller_id(const std::string &id) { this->controller_id_ = id; }

  void setup() override {}
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Queue the enrollment. It goes through the operation queue like all radio work, so it
  /// cannot interleave with a 2W exchange (ADR 0013).
  void press_action() override { this->parent_->send_oneway_enroll(this->controller_id_); }

  std::string controller_id_;
};

}  // namespace home_io_control
}  // namespace esphome
