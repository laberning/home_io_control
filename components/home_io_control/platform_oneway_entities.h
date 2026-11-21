#pragma once

/// @file platform_oneway_entities.h
/// @brief The per-identity 1W entities: command buttons, the enrollment button, the "Last 1W Command" diagnostic
/// text sensor and the optional assumed-state cover.
/// @ingroup hioc_platforms
///
/// All of them are created from the hub's `oneway_controllers:` block, never declared as a
/// `button:` / `text_sensor:` / `cover:` platform entry — for the shared reason, see HubBoundEntity in
/// platform_entity_base.h.
///
/// Each scopes itself to one identity via OneWayControllerBound (parent + controller_id_).

#include <string>
#include <vector>

#include "decisions_oneway.h"
#include "esphome/components/button/button.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
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
  void press_action() override { this->control_port_->send_oneway_action(this->controller_id_, this->action_); }

  OneWayButtonAction action_{OneWayButtonAction::STOP};
};

/// @brief Button entity that registers a configured controller identity as a 1W controller.
///
/// A press sends the gesture its manufacturer expects (OneWayTransmitter::send_enrollment(), ADR
/// 0032). The receiver's half of enrollment is a physical gesture on the receiver side — a
/// multi-second PROG hold on the actuator (Somfy), or a ~1 s Gear press on an already-registered
/// control followed by the product's ready sequence (VELUX); that physical requirement is the real interlock against a
/// stray or unintended enrollment, not a software arming switch — see ADR 0026.
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
  void press_action() override { this->control_port_->send_oneway_enroll(this->controller_id_); }
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

/// @brief Cover entity of a one-way controller identity: open, close and stop, with the state the hub assumes.
///
/// A one-way-only cover has no device address and no read (ADR 0027), so this entity shows what the hub knows of: its
/// identity's own commands that reached the radio, and the presses of the remotes listed under `follows:` that the
/// hub hears (ADR 0117). Both are applied by the pure rule of decisions::oneway_cover_state_after(); there is no
/// position and no travel time, so a move shows its end state at once. A command that did not reach the radio, a
/// frame without a command, a named position and the press of a remote that is not listed change nothing.
///
/// The state is an assumption. It is wrong after a press the hub did not hear and after a command the device
/// ignored (out of range, a desynced counter, a rain or wind sensor's lock), and the hub cannot tell. The state is
/// not persisted (ADR 0018): ESPHome's cover shows open until the first command or press, and assumed state keeps
/// open and close usable in Home Assistant whatever it shows.
/// @ingroup hioc_platforms
class IOHomeOneWayCover : public cover::Cover, public Component, public OneWayControllerBound {
 public:
  /// @brief Subscribe to the identity's own command reports and, when senders are listed, to their presses.
  void setup() override;

  /// @brief Dump the cover's configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Open, close and stop, no position, assumed state.
  [[nodiscard]] cover::CoverTraits get_traits() override;

  /// @brief Add a remote whose presses the cover follows (YAML `follows:`).
  /// @param sender_id Source address, six upper-case hexadecimal characters.
  void add_follow(const std::string &sender_id) { this->follows_.push_back(sender_id); }

 protected:
  /// @brief Queue the command Home Assistant asked for as the identity's own. The state changes when the command's
  /// report says it reached the radio, not here.
  /// @param call The requested operation: stop, or a position of fully open or fully closed.
  void control(const cover::CoverCall &call) override;

  /// @brief Show what a frame asks of the cover, if it asks for anything.
  /// @param wire The frame, with wire values: the identity's own transmission or a followed remote's press.
  void apply_frame_(const OneWayFrameInfo &wire);

  std::vector<std::string> follows_;  ///< Remotes whose presses move the state.
};

}  // namespace home_io_control
}  // namespace esphome
