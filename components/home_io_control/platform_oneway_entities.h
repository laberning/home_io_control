#pragma once

/// @file platform_oneway_entities.h
/// @brief The per-identity 1W entities: command buttons, the enrollment button, the "Last 1W Command" diagnostic
/// text sensor and the optional assumed-state cover and light.
/// @ingroup hioc_platforms
///
/// All of them are created from the hub's `oneway_controllers:` block, never declared as a
/// `button:` / `text_sensor:` / `cover:` platform entry — for the shared reason, see HubBoundEntity in
/// platform_entity_base.h.
///
/// Each scopes itself to one identity via OneWayControllerBound (parent + controller_id_).

#include <optional>
#include <string>
#include <vector>

#include "decisions_oneway.h"
#include "esphome/components/button/button.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "oneway_controller.h"
#include "oneway_transmitter.h"
#include "platform_entity_base.h"
#include "platform_light.h"

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

/// @brief The estimate of a one-way run (ADR 0127) with the bookkeeping an entity needs to show it.
///
/// Holds a decisions::TravelEstimate, the rates it runs at, and what was last published, so that the cover and the
/// light publish a moving level the same way: once at every change of direction, about once a second while moving,
/// and once more when the run arrives. The pure rules stay in decisions_oneway.h; this keeps only the state.
/// @ingroup hioc_platforms
class OneWayTravel {
 public:
  /// @brief Give the estimate its rates and the level it starts at (idle); from then on it is configured.
  /// @param rates Time of a full run each way.
  /// @param start_level Level at boot, 0..1.
  void configure(const decisions::TravelRates &rates, float start_level) {
    this->rates_ = rates;
    this->estimate_ = {start_level, start_level, 0};
    this->configured_ = true;
  }

  /// @brief Whether the rates are set, so a level is estimated.
  [[nodiscard]] bool configured() const { return this->configured_; }
  /// @brief The rates of a full run.
  [[nodiscard]] const decisions::TravelRates &rates() const { return this->rates_; }
  /// @brief The estimate as it was last set; read its level at a time with level().
  [[nodiscard]] const decisions::TravelEstimate &estimate() const { return this->estimate_; }

  /// @brief The estimated level at @p now.
  [[nodiscard]] float level(uint32_t now) const {
    return decisions::travel_level_at(this->estimate_, this->rates_, now);
  }
  /// @brief The direction the estimate runs in at @p now.
  [[nodiscard]] decisions::TravelDirection direction(uint32_t now) const {
    return decisions::travel_direction_at(this->estimate_, this->rates_, now);
  }

  /// @brief Learn of an intent at @p now.
  void learn(const decisions::TravelIntent &intent, uint32_t now) {
    this->estimate_ = decisions::travel_after(this->estimate_, this->rates_, intent, now);
  }

  /// @brief Whether the level is due to be published at @p now. Also settles an arrived run, so that its end stays put
  /// however long the entity then idles.
  [[nodiscard]] bool publish_due(uint32_t now) {
    const auto direction = this->direction(now);
    if (direction == decisions::TravelDirection::IDLE)
      this->estimate_ = decisions::travel_settled(this->estimate_, this->rates_, now);
    return decisions::travel_publish_due(direction, this->published_direction_, this->last_publish_ms_, now,
                                         decisions::TRAVEL_PUBLISH_INTERVAL_MS);
  }

  /// @brief Record that the level was published at @p now.
  void mark_published(uint32_t now) {
    this->published_direction_ = this->direction(now);
    this->last_publish_ms_ = now;
  }

 private:
  bool configured_{false};
  decisions::TravelRates rates_;
  decisions::TravelEstimate estimate_;
  decisions::TravelDirection published_direction_{decisions::TravelDirection::IDLE};  ///< Direction last published.
  uint32_t last_publish_ms_{0};                                                       ///< millis() of the last publish.
};

/// @brief Cover entity of a one-way controller identity: open, close and stop, with the state the hub assumes.
///
/// A one-way-only cover has no device address and no read (ADR 0027), so this entity shows what the hub knows of: its
/// identity's own commands that reached the radio, and the presses of the remotes listed under `follows:` that the
/// hub hears (ADR 0117). Both are applied by the pure rule of decisions::oneway_cover_state_after(); without travel
/// times there is no position, so a move shows its end state at once. A command that did not reach the radio, a
/// frame without a command, a named position and the press of a remote that is not listed change nothing.
///
/// With `open_duration:` and `close_duration:` (set_travel_durations(), ADR 0127) the same inputs drive a
/// decisions::TravelEstimate instead: the cover reports a position, shows opening or closing while the estimate
/// moves, and a position frame travels to its percentage and stops there. A run to an end corrects every earlier
/// error. The estimate is not persisted: after a boot the cover is fully open and idle.
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

  /// @brief Open, close and stop and assumed state; with travel durations also a position.
  [[nodiscard]] cover::CoverTraits get_traits() override;

  /// @brief Publish the moving position about once a second and the end of a run (travel durations only).
  void loop() override;

  /// @brief Give the cover its travel times (YAML `open_duration:` and `close_duration:`, ADR 0127).
  ///
  /// With them the cover estimates a position from the commands and presses it follows; without them it shows only
  /// open or closed.
  /// @param open_ms  Time a full run from closed to open takes.
  /// @param close_ms Time a full run from open to closed takes.
  void set_travel_durations(uint32_t open_ms, uint32_t close_ms) {
    // Fully open at boot, like ESPHome's cover (ADR 0127).
    this->travel_.configure({open_ms, close_ms}, cover::COVER_OPEN);
  }

  /// @brief Add a remote whose presses the cover follows (YAML `follows:`).
  /// @param sender_id Source address, six upper-case hexadecimal characters.
  void add_follow(const std::string &sender_id) { this->follows_.push_back(sender_id); }

 protected:
  /// @brief Queue the command Home Assistant asked for as the identity's own. The state changes when the command's
  /// report says it reached the radio, not here.
  /// @param call The requested operation: stop, or a position. A position is sent as open or close, whichever end
  /// it is nearer (decisions::ONEWAY_COVER_NEARER_END_LEVEL); only a cover with travel times offers values between.
  void control(const cover::CoverCall &call) override;

  /// @brief Show what a frame asks of the cover, if it asks for anything.
  /// @param wire The frame, with wire values: the identity's own transmission or a followed remote's press.
  void apply_frame_(const OneWayFrameInfo &wire);

  /// @brief Move the estimate by an intent and publish it.
  void apply_travel_intent_(const decisions::TravelIntent &intent);

  /// @brief Show the estimate at `now` and remember what was published.
  void publish_estimate_(uint32_t now);

  std::vector<std::string> follows_;  ///< Remotes whose presses move the state.
  /// Where the cover is believed to be, once the travel times are set (opening rises, closing falls).
  OneWayTravel travel_;
};

/// @brief Light entity of a one-way controller identity: on and off, and with `dimmable:` a brightness slider, with the
/// level the hub assumes (ADR 0128).
///
/// A sibling of IOHomeOneWayCover for a light kind. Level 1.0 is on (the wire's 0, full brightness) and 0.0 is off; the
/// identity's open command is on, close is off. The light follows the identity's own commands that reached the radio
/// and the presses of the remotes listed under `follows:`, and shows no more than the hub heard.
///
/// With `on_duration:` and `off_duration:` (set_fade_durations()) the same inputs drive an OneWayTravel: the level
/// moves over the fade time, ends at the commanded end and freezes on a stop, and a position frame sets it at once
/// (decisions::oneway_light_travel_intent_from_frame()). A slider value then becomes commands by
/// decisions::plan_oneway_slider(), the way chosen by set_slider_way(): `timed` sends on or off and, when the estimate
/// reaches the value, one stop that this entity schedules from the report of the command, so that the delay starts
/// when the fade does (if a fade already running passed the value while the command waited, the report sends a stop
/// or the way back instead); `frame` sends one position frame. A new value, a stop sent or heard and a followed
/// remote's press cancel a pending stop, so a stale stop is never sent. Without fade times the light is on or off only:
/// a slider value goes to its nearer end.
///
/// ESPHome's transition stays 0: the device fades by itself, and the fade shown is the estimate of it; a transition
/// asked for anyway sends only its end. The level is an assumption, wrong after a press the hub did not hear or a
/// command the device ignored. Nothing is persisted: after a boot the light shows off.
/// @ingroup hioc_platforms
class IOHomeOneWayLight : public light::LightOutput, public Component, public OneWayControllerBound {
 public:
  /// @brief Subscribe to the identity's own command reports and, when senders are listed, to their presses.
  void setup() override;

  /// @brief Dump the light's configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Publish the moving level about once a second and the end of a fade (fade times only).
  void loop() override;

  /// @brief Brightness when dimmable, on and off otherwise.
  light::LightTraits get_traits() override;

  /// @brief Keep the LightState to publish the level on. ESPHome applies its restore mode right after this call as a
  /// write_state(), which would send a command at boot, so that first write is swallowed.
  /// @param state The light's state object.
  void setup_state(light::LightState *state) override {
    this->state_ = state;
    this->suppress_write_ = true;
  }

  /// @brief Queue the commands Home Assistant's new on, off or brightness value asks for, as described above. The level
  /// shown changes when the command's report says it reached the radio, not here.
  /// @param state The state with the requested values.
  void write_state(light::LightState *state) override;

  /// @brief Give the light a brightness slider (YAML `dimmable:`).
  /// @param dimmable True for brightness, false for on and off only.
  void set_dimmable(bool dimmable) { this->dimmable_ = dimmable; }

  /// @brief Give the light its fade times (YAML `on_duration:` and `off_duration:`, ADR 0128).
  /// @param on_ms  Time a full fade from off to full takes.
  /// @param off_ms Time a full fade from full to off takes.
  void set_fade_durations(uint32_t on_ms, uint32_t off_ms) {
    // Off at boot, like ESPHome's light.
    this->travel_.configure({on_ms, off_ms}, 0.0F);
  }

  /// @brief Choose how a slider value reaches the device (YAML `set_position:`); `timed` by default.
  /// @param way The way.
  void set_slider_way(decisions::OneWaySliderWay way) { this->slider_way_ = way; }

  /// @brief Add a remote whose presses the light follows (YAML `follows:`).
  /// @param sender_id Source address, six upper-case hexadecimal characters.
  void add_follow(const std::string &sender_id) { this->follows_.push_back(sender_id); }

 protected:
  /// @brief Name of the timeout that sends the stop of a timed slider value.
  static constexpr const char *STOP_TIMEOUT = "oneway_light_stop";

  /// @brief Handle the report of one of the identity's own commands: change the shown level if it reached the radio and
  /// arm the stop of a timed slider value if this is its command.
  void on_own_report_(const OneWayCommandReport &report);

  /// @brief Show what a frame asks of the light, if it asks for anything.
  /// @param wire The frame, with wire values: the identity's own transmission or a followed remote's press.
  /// @return The intent that moved the estimate, when the light has fade times and the frame carried one.
  std::optional<decisions::TravelIntent> apply_frame_(const OneWayFrameInfo &wire);

  /// @brief Publish a level on the LightState without sending it back to the device.
  /// @param level Brightness 0..1.
  void show_(float level);

  /// @brief Show the estimate at `now` and remember what was published.
  void publish_estimate_(uint32_t now);

  /// @brief Queue what a slider plan asks for, and keep a timed value until its command's report arms the stop.
  /// @param plan  The plan from decisions::plan_oneway_slider().
  /// @param level The level the plan is for, 0..1.
  void send_plan_(const decisions::OneWaySliderPlan &plan, float level);

  /// @brief Cancel the pending stop of a timed slider value, scheduled or not yet armed.
  void cancel_stop_();

  /// @brief Queue one of the identity's commands and count it as this light's (see unreported_).
  /// @param action The command.
  void queue_action_(OneWayButtonAction action);

  /// @brief Queue a position frame and count it as this light's (see unreported_).
  /// @param position The user's position, 0 full (on) to 100 off.
  void queue_position_(uint8_t position);

  std::vector<std::string> follows_;   ///< Remotes whose presses move the state.
  light::LightState *state_{nullptr};  ///< Where the level is published; null until ESPHome sets the light up.
  bool dimmable_{false};               ///< Whether the slider is offered.
  OneWayTravel travel_;                ///< The level, once the fade times are set (on rises, off falls).
  decisions::OneWaySliderWay slider_way_{decisions::OneWaySliderWay::TIMED};  ///< How a slider value is sent.
  /// A timed slider value whose command waits for its report: the level asked for and the way the command runs.
  struct PendingValue {
    float target{0.0F};  ///< The level asked for, 0..1.
    bool rising{false};  ///< Whether the command sent for it is on (open), else off (close).
  };
  /// The latest timed slider value while its command waits for its report; the report arms the stop.
  std::optional<PendingValue> pending_value_;
  /// Commands this light queued whose report has not come. The queue reports each command once, in order, so the
  /// report that brings this to zero is the one of the light's latest command.
  uint32_t unreported_{0};
  /// Guard so that publishing the level does not echo back as a new command (LightState re-enters write_state()).
  bool suppress_write_{false};
};

}  // namespace home_io_control
}  // namespace esphome
