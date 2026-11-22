/// @file platform_oneway_entities.cpp
/// @brief The per-identity 1W entities: command buttons, enrollment button, last-command sensor, cover, light.
/// @ingroup hioc_platforms

#include "platform_oneway_entities.h"

#include <algorithm>
#include <cinttypes>
#include <utility>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "proto_device_kinds.h"

namespace esphome {
namespace home_io_control {

std::string format_oneway_command_report(const OneWayCommandReport &report) {
  // "not sent" rather than "failed": the hub knows only that nothing left the radio. It never
  // knows that a command failed, because it never learns that one succeeded either.
  if (!report.sequence_reserved)
    return "not sent (no sequence reserved)";

  std::string summary = report.intent.empty() ? "command" : report.intent;
  summary += " -> ";
  summary += device_type_name(report.target_type);
  summary += " seq ";
  summary += std::to_string(report.sequence);
  if (!report.transmitted)
    summary += " (not sent)";
  return summary;
}

void IOHomeOneWayCommandButton::dump_config() {
  ESP_LOGCONFIG("home_io_control.button", "IO-Homecontrol 1W Command Button");
  ESP_LOGCONFIG("home_io_control.button", "  Controller: %s", this->controller_id_.c_str());
  ESP_LOGCONFIG("home_io_control.button", "  Action: %s", oneway_button_action_name(this->action_));
}

void IOHomeOneWayEnrollButton::dump_config() {
  ESP_LOGCONFIG("home_io_control.button", "IO-Homecontrol 1W Enroll Button");
  ESP_LOGCONFIG("home_io_control.button", "  Controller: %s", this->controller_id_.c_str());
}

void IOHomeOneWayLastCommandTextSensor::setup() {
  if (this->control_port_ == nullptr)
    return;
  this->control_port_->add_oneway_command_report_callback([this](const OneWayCommandReport &report) {
    // Every sensor sees every report; each keeps only its own identity's. Filtering here rather
    // than at the hub keeps the hub from having to know which sensors exist.
    if (report.controller_id != this->controller_id_)
      return;
    this->publish_state(format_oneway_command_report(report));
  });
}

void IOHomeOneWayLastCommandTextSensor::dump_config() {
  ESP_LOGCONFIG("home_io_control.text_sensor", "IO-Homecontrol Last 1W Command Sensor");
  ESP_LOGCONFIG("home_io_control.text_sensor", "  Controller: %s", this->controller_id_.c_str());
}

// === OneWaySlider ===

void OneWaySlider::ask(float level) {
  this->cancel();
  this->send_plan_(
      decisions::plan_oneway_slider(this->travel_->estimate(), this->travel_->rates(), millis(), level, this->way_),
      level);
}

void OneWaySlider::send_plan_(const decisions::OneWaySliderPlan &plan, float level) {
  using Send = decisions::OneWaySliderPlan::Send;
  switch (plan.send) {
    case Send::OPEN:
    case Send::CLOSE: {
      const uint32_t ticket =
          this->queue_action(plan.send == Send::OPEN ? OneWayButtonAction::OPEN : OneWayButtonAction::CLOSE);
      // The stop is armed by this command's report and timed from the estimate then, when the run has begun.
      if (plan.stop_after_ms.has_value())
        this->pending_value_ = PendingValue{std::clamp(level, 0.0F, 1.0F), plan.send == Send::OPEN, ticket};
      break;
    }
    case Send::STOP:
      this->queue_action(OneWayButtonAction::STOP);
      break;
    case Send::POSITION:
      this->queue_position(plan.position_percent);
      break;
    case Send::NOTHING:
    default:
      break;
  }
}

void OneWaySlider::cancel() {
  this->pending_value_.reset();
  this->cancel_stop_();
}

uint32_t OneWaySlider::queue_action(OneWayButtonAction action) {
  return this->port_->send_oneway_action(this->controller_id_, action);
}

uint32_t OneWaySlider::queue_position(uint8_t position) {
  return this->port_->send_oneway_position(this->controller_id_, position);
}

void OneWaySlider::on_own_report(const OneWayCommandReport &report) {
  // The report of the pending value's own command, told by its ticket, is the one that arms the stop. Any other report
  // (a button, an action, an earlier value) leaves the pending value waiting for its own.
  std::optional<PendingValue> pending;
  if (this->pending_value_.has_value() && report.ticket == this->pending_value_->ticket)
    pending = std::exchange(this->pending_value_, std::nullopt);
  // A burst that never reached the radio changes nothing, and neither does enrollment traffic (no intent).
  if (!decisions::follows_own_oneway_command(report.transmitted, report.wire.has_intent))
    return;
  // Any command of the identity supersedes a stop already armed; the latest command of a timed value arms its own.
  this->cancel_stop_();
  const auto intent = this->apply_frame_(report.wire);
  if (!pending.has_value() || !intent.has_value() || intent->kind != decisions::TravelIntent::Kind::GO_TO)
    return;
  const bool rising = intent->level >= 1.0F;
  if (rising != pending->rising)  // the frame that went out is not the command that was queued: nothing to time
    return;
  // Timed from the estimate as the run begins, by the same rule that chose the command.
  const auto plan = decisions::plan_oneway_slider(this->travel_->estimate(), this->travel_->rates(), millis(),
                                                  pending->target, decisions::OneWaySliderWay::TIMED);
  const auto sent = rising ? decisions::OneWaySliderPlan::Send::OPEN : decisions::OneWaySliderPlan::Send::CLOSE;
  if (plan.send == sent && plan.stop_after_ms.has_value()) {
    this->arm_stop_(*plan.stop_after_ms, [this]() { this->queue_action(OneWayButtonAction::STOP); });
    return;
  }
  // A run already going passed the value while the command waited for the radio: send what the rule asks for from
  // here, a stop at the value or the way back, whose own report then arms its stop.
  this->send_plan_(plan, pending->target);
}

// === IOHomeOneWayCover ===

void IOHomeOneWayCover::setup() {
  if (this->control_port_ == nullptr)
    return;
  this->slider_.attach(
      *this->control_port_, this->controller_id_, this->travel_,
      [this](uint32_t delay_ms, std::function<void()> stop) {
        this->set_timeout(STOP_TIMEOUT, delay_ms, std::move(stop));
      },
      [this]() { this->cancel_timeout(STOP_TIMEOUT); },
      [this](const OneWayFrameInfo &wire) { return this->apply_frame_(wire); });
  this->control_port_->add_oneway_command_report_callback([this](const OneWayCommandReport &report) {
    // Every cover sees every report; each keeps only its own identity's. A burst that never reached the radio changes
    // nothing, and neither does enrollment traffic (its frames carry no intent).
    if (report.controller_id == this->controller_id_)
      this->slider_.on_own_report(report);
  });
  if (!this->follows_.empty()) {
    this->control_port_->add_oneway_press_callback(this->follows_, [this](const OneWayFrameInfo &press) {
      // A remote's press supersedes the stop that was planned for a value of ours.
      this->slider_.cancel();
      this->apply_frame_(press);
    });
  }
}

void IOHomeOneWayCover::dump_config() {
  ESP_LOGCONFIG("home_io_control.cover", "IO-Homecontrol 1W Cover");
  ESP_LOGCONFIG("home_io_control.cover", "  Controller: %s", this->controller_id_.c_str());
  for (const auto &sender : this->follows_)
    ESP_LOGCONFIG("home_io_control.cover", "  Follows: %s", sender.c_str());
  if (this->travel_.configured()) {
    ESP_LOGCONFIG("home_io_control.cover", "  Open duration: %" PRIu32 " ms", this->travel_.rates().rise_ms);
    ESP_LOGCONFIG("home_io_control.cover", "  Close duration: %" PRIu32 " ms", this->travel_.rates().fall_ms);
    ESP_LOGCONFIG("home_io_control.cover", "  Set position: %s",
                  this->slider_.way() == decisions::OneWaySliderWay::FRAME ? "frame" : "timed");
  }
}

cover::CoverTraits IOHomeOneWayCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(this->travel_.configured());
  traits.set_supports_stop(true);
  traits.set_is_assumed_state(
      true);  // No read exists: the shown state is what the hub heard, never what the device did
  return traits;
}

void IOHomeOneWayCover::loop() {
  if (!this->travel_.configured())
    return;
  const uint32_t now = millis();
  if (this->travel_.publish_due(now))
    this->publish_estimate_(now);
}

void IOHomeOneWayCover::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    // A stop supersedes the stop that was planned for a value.
    this->slider_.cancel();
    this->slider_.queue_action(OneWayButtonAction::STOP);
    return;
  }
  const auto &position = call.get_position();
  if (!position.has_value())
    return;
  if (this->travel_.configured()) {
    this->slider_.ask(*position);
    return;
  }
  // Without travel times a slider value goes to its nearer end: the device is only sent open, close and stop here.
  this->slider_.queue_action(*position >= decisions::ONEWAY_COVER_NEARER_END_LEVEL ? OneWayButtonAction::OPEN
                                                                                   : OneWayButtonAction::CLOSE);
}

std::optional<decisions::TravelIntent> IOHomeOneWayCover::apply_frame_(const OneWayFrameInfo &wire) {
  const bool inverted = this->control_port_->oneway_identity_inverted(this->controller_id_);
  if (this->travel_.configured()) {
    const auto intent = decisions::oneway_cover_travel_intent_from_frame(wire, inverted);
    if (intent.has_value())
      this->apply_travel_intent_(*intent);
    return intent;
  }
  const auto intent = decisions::oneway_cover_intent_from_frame(wire, inverted);
  if (!intent.has_value())
    return std::nullopt;
  // A stop leaves the cover somewhere between the ends. Home Assistant's position-less cover has no such state,
  // so it shows open, as it does for a two-way cover that is not fully closed.
  this->position = decisions::oneway_cover_state_after(*intent) == decisions::OneWayCoverState::CLOSED
                       ? cover::COVER_CLOSED
                       : cover::COVER_OPEN;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->publish_state();
  return std::nullopt;
}

void IOHomeOneWayCover::apply_travel_intent_(const decisions::TravelIntent &intent) {
  const uint32_t now = millis();
  this->travel_.learn(intent, now);
  this->publish_estimate_(now);
}

void IOHomeOneWayCover::publish_estimate_(uint32_t now) {
  const auto direction = this->travel_.direction(now);
  this->position = this->travel_.level(now);
  switch (direction) {
    case decisions::TravelDirection::UP:
      this->current_operation = cover::COVER_OPERATION_OPENING;
      break;
    case decisions::TravelDirection::DOWN:
      this->current_operation = cover::COVER_OPERATION_CLOSING;
      break;
    case decisions::TravelDirection::IDLE:
    default:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      break;
  }
  this->travel_.mark_published(now);
  this->publish_state();
}

// === IOHomeOneWayLight ===

void IOHomeOneWayLight::setup() {
  if (this->control_port_ == nullptr)
    return;
  this->slider_.attach(
      *this->control_port_, this->controller_id_, this->travel_,
      [this](uint32_t delay_ms, std::function<void()> stop) {
        this->set_timeout(STOP_TIMEOUT, delay_ms, std::move(stop));
      },
      [this]() { this->cancel_timeout(STOP_TIMEOUT); },
      [this](const OneWayFrameInfo &wire) { return this->apply_frame_(wire); });
  this->control_port_->add_oneway_command_report_callback([this](const OneWayCommandReport &report) {
    // Every light sees every report; each keeps only its own identity's.
    if (report.controller_id == this->controller_id_)
      this->slider_.on_own_report(report);
  });
  if (!this->follows_.empty()) {
    this->control_port_->add_oneway_press_callback(this->follows_, [this](const OneWayFrameInfo &press) {
      // A remote's press supersedes the stop that was planned for a value of ours.
      this->slider_.cancel();
      this->apply_frame_(press);
    });
  }
}

void IOHomeOneWayLight::dump_config() {
  ESP_LOGCONFIG("home_io_control.light", "IO-Homecontrol 1W %s Light", this->dimmable_ ? "Dimmable" : "Binary");
  ESP_LOGCONFIG("home_io_control.light", "  Controller: %s", this->controller_id_.c_str());
  for (const auto &sender : this->follows_)
    ESP_LOGCONFIG("home_io_control.light", "  Follows: %s", sender.c_str());
  if (this->travel_.configured()) {
    ESP_LOGCONFIG("home_io_control.light", "  On duration: %" PRIu32 " ms", this->travel_.rates().rise_ms);
    ESP_LOGCONFIG("home_io_control.light", "  Off duration: %" PRIu32 " ms", this->travel_.rates().fall_ms);
    ESP_LOGCONFIG("home_io_control.light", "  Set position: %s",
                  this->slider_.way() == decisions::OneWaySliderWay::FRAME ? "frame" : "timed");
  }
}

light::LightTraits IOHomeOneWayLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({this->dimmable_ ? light::ColorMode::BRIGHTNESS : light::ColorMode::ON_OFF});
  return traits;
}

void IOHomeOneWayLight::loop() {
  if (!this->travel_.configured())
    return;
  const uint32_t now = millis();
  if (this->travel_.publish_due(now))
    this->publish_estimate_(now);
}

void IOHomeOneWayLight::write_state(light::LightState *state) {
  if (this->suppress_write_) {
    this->suppress_write_ = false;
    return;
  }
  // A transition Home Assistant asks for (a scene's `transition:`) writes every loop on its way; each write would be a
  // one-way burst and a rolling sequence. Only its end, written once the transition is over, is sent.
  if (state->is_transformer_active())
    return;
  float level = 0.0F;
  if (this->dimmable_) {
    // The linear value, not the gamma-corrected one: the device has its own dimming curve (see IOHomeLight).
    state->current_values.as_brightness(&level);
  } else {
    bool on = false;
    state->current_values_as_binary(&on);
    level = on ? 1.0F : 0.0F;
  }

  if (!this->travel_.configured()) {
    // No fade times: only the ends can be sent.
    this->slider_.cancel();
    this->slider_.queue_action(level >= decisions::ONEWAY_COVER_NEARER_END_LEVEL ? OneWayButtonAction::OPEN
                                                                                 : OneWayButtonAction::CLOSE);
    return;
  }
  this->slider_.ask(level);
}

std::optional<decisions::TravelIntent> IOHomeOneWayLight::apply_frame_(const OneWayFrameInfo &wire) {
  const bool inverted = this->control_port_->oneway_identity_inverted(this->controller_id_);
  if (this->travel_.configured()) {
    const auto intent = decisions::oneway_light_travel_intent_from_frame(wire, inverted);
    if (intent.has_value()) {
      const uint32_t now = millis();
      this->travel_.learn(*intent, now);
      this->publish_estimate_(now);
    }
    return intent;
  }
  // No fade times: on or off by the nearer end. A stop leaves a level the hub cannot know, so it shows none.
  const auto intent = decisions::oneway_cover_intent_from_frame(wire, inverted);
  if (intent.has_value() && *intent != decisions::OneWayCoverIntent::STOP)
    this->show_(*intent == decisions::OneWayCoverIntent::OPEN ? 1.0F : 0.0F);
  return std::nullopt;
}

void IOHomeOneWayLight::show_(float level) {
  if (this->state_ == nullptr)
    return;
  publish_light_reading(this->state_, level > 0.0F, this->dimmable_ ? std::optional<float>(level) : std::nullopt,
                        this->suppress_write_);
}

void IOHomeOneWayLight::publish_estimate_(uint32_t now) {
  this->show_(this->travel_.level(now));
  this->travel_.mark_published(now);
}

}  // namespace home_io_control
}  // namespace esphome
