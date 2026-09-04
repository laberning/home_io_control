/// @file platform_companion_sensors.cpp
/// @brief The five auto-generated per-device diagnostic companion sensors.
/// @ingroup hioc_platforms

#include "platform_companion_sensors.h"

#include "hub_internal.h"
#include "proto_constants.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

// Each dump_config() keeps its own log tag as a `static` function-local constant: the ESPHome
// LOG_SENSOR / LOG_TEXT_SENSOR macros expand to a bare `TAG` identifier, so one shared file-scope
// constant could not carry five different values. Values are unchanged from the former per-sensor
// files. (`static` so the name resolves as a StaticConstant, i.e. UPPER_CASE, under clang-tidy's
// identifier-naming check rather than as a lower_case local.)

namespace {

constexpr float MS_PER_SECOND = 1000.0F;
// How often the Last Contact heartbeat re-publishes the age while the device stays quiet.
// Frame-driven updates alone would freeze the entity's value at whatever it was the moment
// traffic stopped; without this it can't count up in Home Assistant between frames.
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;

}  // namespace

void IOHomeRssiSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (const int16_t ema_dbm = device_rssi_ema_dbm(dev); ema_dbm != RSSI_UNKNOWN_DBM)
      this->publish_state(ema_dbm);
  });
}

void IOHomeRssiSensor::dump_config() {
  static const char *const TAG = "home_io_control.rssi";
  LOG_SENSOR("", "IO-Homecontrol RSSI", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

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
  static const char *const TAG = "home_io_control.last_contact";
  LOG_SENSOR("", "IO-Homecontrol Last Contact", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeExchangeFailuresSensor::setup() {
  this->register_companion_binding_(
      [this](const IoDevice &dev) { this->publish_state(static_cast<float>(dev.exchange_timeout_count)); });
}

void IOHomeExchangeFailuresSensor::dump_config() {
  static const char *const TAG = "home_io_control.exchange_failures";
  LOG_SENSOR("", "IO-Homecontrol Exchange Failures", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeDeviceNameTextSensor::setup() {
  if (this->parent_ == nullptr)
    return;

  this->register_companion_binding_([this](const IoDevice &dev) { this->publish_state(dev.name); });

  this->set_timeout("init_name", INITIAL_STATUS_REQUEST_DELAY_MS,
                    [this]() { this->parent_->queue_request_device_name(this->device_id_); });
}

void IOHomeDeviceNameTextSensor::dump_config() {
  static const char *const TAG = "home_io_control.device_name";
  LOG_TEXT_SENSOR("", "IO-Homecontrol Device Name", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeActiveIssueTextSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // Empty string when no result is recorded; otherwise the symbolic result name.
    this->publish_state(dev.last_result_code == 0 ? "" : command_result_name(dev.last_result_code));
  });
}

void IOHomeActiveIssueTextSensor::dump_config() {
  static const char *const TAG = "home_io_control.active_issue";
  LOG_TEXT_SENSOR("", "IO-Homecontrol Active Issue", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
