/// @file platform_device_name_text_sensor.cpp
/// @brief Diagnostic text sensor exposing the stored IO-Homecontrol device name.
/// @ingroup hioc_platforms

#include "platform_device_name_text_sensor.h"

#include "hub_internal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.device_name";

void IOHomeDeviceNameTextSensor::setup() {
  if (this->parent_ == nullptr)
    return;

  this->parent_->register_device_callback(
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });

  if (const auto *dev = this->parent_->get_device(this->device_id_); dev != nullptr)
    this->publish_state(dev->name);

  this->set_timeout("init_name", detail::INITIAL_STATUS_REQUEST_DELAY_MS,
                    [this]() { this->parent_->queue_request_device_name(this->device_id_); });
}

void IOHomeDeviceNameTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "IO-Homecontrol Device Name", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeDeviceNameTextSensor::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;

  this->publish_state(dev.name);
}

}  // namespace home_io_control
}  // namespace esphome