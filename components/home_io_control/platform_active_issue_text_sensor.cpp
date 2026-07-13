/// @file platform_active_issue_text_sensor.cpp
/// @brief Diagnostic text sensor exposing a device's currently outstanding CMD_ERROR_RESP reason.
/// @ingroup hioc_platforms

#include "platform_active_issue_text_sensor.h"

#include "proto_constants.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.active_issue";

void IOHomeActiveIssueTextSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // Empty string when no result is recorded; otherwise the symbolic result name.
    this->publish_state(dev.last_result_code == 0 ? "" : command_result_name(dev.last_result_code));
  });
}

void IOHomeActiveIssueTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "IO-Homecontrol Active Issue", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
