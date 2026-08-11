/// @file platform_oneway_last_command_text_sensor.cpp
/// @brief Diagnostic text sensor reporting the last 1W command a controller identity transmitted.
/// @ingroup hioc_platforms

#include "platform_oneway_last_command_text_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

std::string format_oneway_command_report(const OneWayCommandReport &report) {
  // "not sent" rather than "failed": the hub knows only that nothing left the radio. It never
  // knows that a command failed, because it never learns that one succeeded either.
  if (report.sequence == 0 && !report.transmitted)
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

void IOHomeOneWayLastCommandTextSensor::setup() {
  if (this->parent_ == nullptr)
    return;
  this->parent_->add_oneway_command_report_callback([this](const OneWayCommandReport &report) {
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

}  // namespace home_io_control
}  // namespace esphome
