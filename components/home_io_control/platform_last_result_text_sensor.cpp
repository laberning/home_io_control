/// @file platform_last_result_text_sensor.cpp
/// @brief Diagnostic text sensor exposing the device's last CMD_ERROR_RESP result reason.
/// @ingroup hioc_platforms

#include "platform_last_result_text_sensor.h"

#include "proto_constants.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.last_result";

namespace {

/// @brief Render a device's last_result_code as the sensor's published string.
/// @param dev Device whose cached result to render.
/// @return Empty string when no result is recorded; otherwise command_result_name().
std::string render_last_result(const IoDevice &dev) {
  return dev.last_result_code == 0 ? "" : command_result_name(dev.last_result_code);
}

}  // namespace

void IOHomeLastResultTextSensor::setup() {
  if (this->parent_ == nullptr)
    return;

  this->parent_->register_device_callback(
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });

  if (const auto *dev = this->parent_->get_device(this->device_id_); dev != nullptr)
    this->publish_state(render_last_result(*dev));
}

void IOHomeLastResultTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "IO-Homecontrol Last Result", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeLastResultTextSensor::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;

  this->publish_state(render_last_result(dev));
}

}  // namespace home_io_control
}  // namespace esphome
