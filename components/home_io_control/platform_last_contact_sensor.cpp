/// @file platform_last_contact_sensor.cpp
/// @brief Diagnostic sensor exposing seconds elapsed since a device's last received frame.
/// @ingroup hioc_platforms

#include "platform_last_contact_sensor.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.last_contact";

namespace {

constexpr float MS_PER_SECOND = 1000.0F;
// How often the heartbeat re-publishes the age while the device stays quiet. Frame-driven
// updates alone would freeze the entity's value at whatever it was the moment traffic stopped;
// without this it can't count up in Home Assistant between frames.
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;

}  // namespace

void IOHomeLastContactSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) { this->publish_age_(dev); });

  this->set_interval("heartbeat", HEARTBEAT_INTERVAL_MS, [this]() {
    if (this->parent_ == nullptr)
      return;
    if (const auto *dev = this->parent_->get_device(this->device_id_); dev != nullptr)
      this->publish_age_(*dev);
  });
}

void IOHomeLastContactSensor::publish_age_(const IoDevice &dev) {
  if (dev.last_seen_ms != 0)
    this->publish_state(static_cast<float>(millis() - dev.last_seen_ms) / MS_PER_SECOND);
}

void IOHomeLastContactSensor::dump_config() {
  LOG_SENSOR("", "IO-Homecontrol Last Contact", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
