/// @file platform_pairing_result_text_sensor.cpp
/// @brief Diagnostic text sensor exposing the "Last Pairing Result" string.
/// @ingroup hioc_platforms

#include "platform_pairing_result_text_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.pairing_result";

void IOHomePairingResultTextSensor::setup() {
  if (this->parent_ == nullptr)
    return;

  this->parent_->set_pairing_result_callback([this]() { this->on_pairing_result_(); });
}

void IOHomePairingResultTextSensor::dump_config() { LOG_TEXT_SENSOR("", "IO-Homecontrol Last Pairing Result", this); }

void IOHomePairingResultTextSensor::on_pairing_result_() {
  if (this->parent_ == nullptr)
    return;
  this->publish_state(this->parent_->pairing_telemetry().result_sensor_string());
}

}  // namespace home_io_control
}  // namespace esphome
