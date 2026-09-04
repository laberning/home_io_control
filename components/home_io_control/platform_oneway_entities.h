#pragma once

/// @file platform_oneway_entities.h
/// @brief The per-identity 1W entities: command buttons, the enrollment button, and the
/// "Last 1W Command" diagnostic text sensor.
/// @ingroup hioc_platforms
///
/// All three are created from the hub's `oneway_controllers:` block, never declared as a
/// `button:` / `text_sensor:` platform entry — for the shared reason, see HubBoundEntity in
/// platform_entity_base.h.
///
/// All three scope themselves to one identity via OneWayControllerBound (parent + controller_id_).

#include <string>

#include "esphome/components/button/button.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "oneway_controller.h"
#include "oneway_transmitter.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Build the sensor string for one command report.
///
/// Pure and free-standing so the wording is unit-testable: the host ESP_LOG stub discards its
/// arguments, so a formatter buried in a publish call could not be asserted on at all.
/// @param report The attempt to describe.
/// @return Human-readable summary, e.g. `STOP -> awning seq 1234` or `STOP -> awning seq 1234 (not sent)`.
std::string format_oneway_command_report(const OneWayCommandReport &report);

/// @brief Button entity that sends one 1W command as a configured controller identity.
///
/// A press is fire-and-forget. Nothing replies, so a press that a device ignores looks exactly
/// like one it obeyed — IOHomeOneWayLastCommandTextSensor reports what was transmitted, which is
/// the only half of that the hub can know.
/// @ingroup hioc_platforms
class IOHomeOneWayCommandButton : public button::Button, public Component, public OneWayControllerBound {
 public:
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

  OneWayButtonAction action_{OneWayButtonAction::STOP};
};

/// @brief Button entity that registers a configured controller identity as a 1W controller.
///
/// A press sends `0x39` then `0x30`, back to back (OneWayTransmitter::send_enrollment()) — the
/// documented 1W pairing handshake. The receiver's half of enrollment is a physical 2s PROG hold
/// on the actuator itself; that physical requirement is the real interlock against a stray or
/// unintended enrollment, not a software arming switch — see ADR 0026.
/// @ingroup hioc_platforms
class IOHomeOneWayEnrollButton : public button::Button, public Component, public OneWayControllerBound {
 public:
  void setup() override {}
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Queue the enrollment. It goes through the operation queue like all radio work, so it
  /// cannot interleave with a 2W exchange (ADR 0013).
  void press_action() override { this->parent_->send_oneway_enroll(this->controller_id_); }
};

/// @brief Diagnostic text sensor: what this identity last put on air.
///
/// **This is the only feedback 1W transmit can ever give.** There is no reply frame, so a command
/// a device ignored is indistinguishable on the radio from one it obeyed. A user with a wrong key,
/// a desynced counter or a missing enrollment would otherwise see nothing at all — not an error,
/// not a timeout, nothing.
///
/// So this sensor deliberately does *not* claim the device acted, and its text must never be
/// worded as if it did. It reports what the hub transmitted, which is the half that is knowable,
/// and it surfaces the **sequence** used so a stuck user can judge whether the counter is the
/// problem and whether `initial_sequence:` needs bumping.
/// @ingroup hioc_platforms
class IOHomeOneWayLastCommandTextSensor : public text_sensor::TextSensor,
                                          public Component,
                                          public OneWayControllerBound {
 public:
  /// @brief Subscribe to the hub's per-command reports.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace home_io_control
}  // namespace esphome
