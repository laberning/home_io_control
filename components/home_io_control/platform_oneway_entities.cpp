/// @file platform_oneway_entities.cpp
/// @brief The per-identity 1W entities: command buttons, enrollment button, last-command sensor, cover.
/// @ingroup hioc_platforms

#include "platform_oneway_entities.h"

#include <cinttypes>

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

// === IOHomeOneWayCover ===

void IOHomeOneWayCover::setup() {
  if (this->control_port_ == nullptr)
    return;
  this->control_port_->add_oneway_command_report_callback([this](const OneWayCommandReport &report) {
    // Every cover sees every report; each keeps only its own identity's. A burst that never reached the radio
    // changes nothing, and neither does enrollment traffic (its frames carry no intent).
    if (report.controller_id != this->controller_id_ ||
        !decisions::follows_own_oneway_command(report.transmitted, report.wire.has_intent))
      return;
    this->apply_frame_(report.wire);
  });
  if (!this->follows_.empty()) {
    this->control_port_->add_oneway_press_callback(this->follows_,
                                                   [this](const OneWayFrameInfo &press) { this->apply_frame_(press); });
  }
}

void IOHomeOneWayCover::dump_config() {
  ESP_LOGCONFIG("home_io_control.cover", "IO-Homecontrol 1W Cover");
  ESP_LOGCONFIG("home_io_control.cover", "  Controller: %s", this->controller_id_.c_str());
  for (const auto &sender : this->follows_)
    ESP_LOGCONFIG("home_io_control.cover", "  Follows: %s", sender.c_str());
  if (this->has_durations_) {
    ESP_LOGCONFIG("home_io_control.cover", "  Open duration: %" PRIu32 " ms", this->rates_.rise_ms);
    ESP_LOGCONFIG("home_io_control.cover", "  Close duration: %" PRIu32 " ms", this->rates_.fall_ms);
  }
}

cover::CoverTraits IOHomeOneWayCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(this->has_durations_);
  traits.set_supports_stop(true);
  traits.set_is_assumed_state(
      true);  // No read exists: the shown state is what the hub heard, never what the device did
  return traits;
}

void IOHomeOneWayCover::loop() {
  if (!this->has_durations_)
    return;
  const uint32_t now = millis();
  // Settle an arrived run so that its end stays put however long the cover then idles.
  const auto direction = decisions::travel_direction_at(this->estimate_, this->rates_, now);
  if (direction == decisions::TravelDirection::IDLE)
    this->estimate_ = decisions::travel_settled(this->estimate_, this->rates_, now);
  if (decisions::travel_publish_due(direction, this->published_direction_, this->last_publish_ms_, now,
                                    decisions::TRAVEL_PUBLISH_INTERVAL_MS))
    this->publish_estimate_(now);
}

void IOHomeOneWayCover::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    this->control_port_->send_oneway_action(this->controller_id_, OneWayButtonAction::STOP);
    return;
  }
  const auto &position = call.get_position();
  if (!position.has_value())
    return;
  // A slider value goes to its nearer end: the device is only sent open, close and stop here.
  this->control_port_->send_oneway_action(this->controller_id_, *position >= decisions::ONEWAY_COVER_NEARER_END_LEVEL
                                                                    ? OneWayButtonAction::OPEN
                                                                    : OneWayButtonAction::CLOSE);
}

void IOHomeOneWayCover::apply_frame_(const OneWayFrameInfo &wire) {
  const bool inverted = this->control_port_->oneway_identity_inverted(this->controller_id_);
  if (this->has_durations_) {
    const auto intent = decisions::oneway_cover_travel_intent_from_frame(wire, inverted);
    if (intent.has_value())
      this->apply_travel_intent_(*intent);
    return;
  }
  const auto intent = decisions::oneway_cover_intent_from_frame(wire, inverted);
  if (!intent.has_value())
    return;
  // A stop leaves the cover somewhere between the ends. Home Assistant's position-less cover has no such state,
  // so it shows open, as it does for a two-way cover that is not fully closed.
  this->position = decisions::oneway_cover_state_after(*intent) == decisions::OneWayCoverState::CLOSED
                       ? cover::COVER_CLOSED
                       : cover::COVER_OPEN;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->publish_state();
}

void IOHomeOneWayCover::apply_travel_intent_(const decisions::TravelIntent &intent) {
  const uint32_t now = millis();
  this->estimate_ = decisions::travel_after(this->estimate_, this->rates_, intent, now);
  this->publish_estimate_(now);
}

void IOHomeOneWayCover::publish_estimate_(uint32_t now) {
  this->published_direction_ = decisions::travel_direction_at(this->estimate_, this->rates_, now);
  this->position = decisions::travel_level_at(this->estimate_, this->rates_, now);
  switch (this->published_direction_) {
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
  this->last_publish_ms_ = now;
  this->publish_state();
}

}  // namespace home_io_control
}  // namespace esphome
