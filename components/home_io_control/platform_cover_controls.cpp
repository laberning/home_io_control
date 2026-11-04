/// @file platform_cover_controls.cpp
/// @brief Generated per-cover auxiliary entities: command button, silent-operation toggle, travel speed select,
/// rain and battery sensors.
/// @ingroup hioc_platforms

#include "platform_cover_controls.h"

#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace home_io_control {

// One tag for the merged command button. The former per-button tags (home_io_control.favorite_button
// / home_io_control.vent_button) both retire with the class merge; a user greps for the entity by
// name, not by log tag, and this reads correctly for either command.
static const char *const TAG_COMMAND_BUTTON = "home_io_control.cover_command_button";
static const char *const TAG_SILENT = "home_io_control.cover.silent";
static const char *const TAG_TRAVEL_SPEED = "home_io_control.cover.travel_speed";
static const char *const TAG_RAIN = "home_io_control.cover.rain_sensor";
static const char *const TAG_BATTERY = "home_io_control.cover.battery";
static const char *const TAG_SUN_LEVEL = "home_io_control.cover.sun_level";
static const char *const TAG_BATTERY_LEVEL = "home_io_control.cover.battery_level";

void IOHomeCoverCommandButton::press_action() {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG_COMMAND_BUTTON, "Ignoring %s press because the parent hub is not configured",
             cover_command_name(this->command_));
    return;
  }

  if (this->parent_->get_device(this->device_id_) == nullptr) {
    ESP_LOGW(TAG_COMMAND_BUTTON, "Ignoring %s press for %s because the device is not registered yet",
             cover_command_name(this->command_), this->device_id_.c_str());
    return;
  }

  this->parent_->queue_device_command(this->device_id_, this->command_);
}

void IOHomeCoverCommandButton::dump_config() {
  ESP_LOGCONFIG(TAG_COMMAND_BUTTON, "IO-Homecontrol %s Position Button", cover_command_name(this->command_));
  ESP_LOGCONFIG(TAG_COMMAND_BUTTON, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeCoverSilentSwitch::setup() {
  // The YAML value was already applied to the device at registration; mirror it outward so Home
  // Assistant does not start out disagreeing with what the hub will actually send.
  if (this->parent_ == nullptr)
    return;
  if (const IoDevice *dev = this->parent_->get_device(this->device_id_); dev != nullptr)
    this->publish_state(dev->travel_speed == TravelSpeed::SILENT);
}

void IOHomeCoverSilentSwitch::write_state(bool state) {
  if (this->parent_ == nullptr)
    return;
  this->parent_->set_device_travel_speed(this->device_id_, state ? TravelSpeed::SILENT : TravelSpeed::FAST);
  this->publish_state(state);
  ESP_LOGI(TAG_SILENT, "Device %s: silent operation %s", this->device_id_.c_str(), state ? "on" : "off");
}

void IOHomeCoverSilentSwitch::dump_config() {
  ESP_LOGCONFIG(TAG_SILENT, "IO-Homecontrol Silent Operation:");
  ESP_LOGCONFIG(TAG_SILENT, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeCoverTravelSpeedSelect::setup() {
  // The YAML value was already applied to the device at registration; mirror it outward so Home
  // Assistant does not start out disagreeing with what the hub will actually send.
  if (this->parent_ == nullptr)
    return;
  if (const IoDevice *dev = this->parent_->get_device(this->device_id_); dev != nullptr)
    this->publish_state(travel_speed_name(dev->travel_speed));
}

void IOHomeCoverTravelSpeedSelect::control(const std::string &value) {
  if (this->parent_ == nullptr)
    return;
  TravelSpeed speed = TravelSpeed::FAST;
  if (!parse_travel_speed(value, speed)) {
    ESP_LOGW(TAG_TRAVEL_SPEED, "Device %s: ignoring unknown travel speed '%s'", this->device_id_.c_str(),
             value.c_str());
    return;
  }
  this->parent_->set_device_travel_speed(this->device_id_, speed);
  this->publish_state(value);
  ESP_LOGI(TAG_TRAVEL_SPEED, "Device %s: travel speed %s", this->device_id_.c_str(), travel_speed_name(speed));
}

void IOHomeCoverTravelSpeedSelect::dump_config() {
  ESP_LOGCONFIG(TAG_TRAVEL_SPEED, "IO-Homecontrol Travel Speed:");
  ESP_LOGCONFIG(TAG_TRAVEL_SPEED, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeRainBinarySensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) { this->publish_rain_state_(dev.rain_sensor); });
}

void IOHomeRainBinarySensor::publish_rain_state_(RainSensorState state) {
  switch (state) {
    case RainSensorState::RAIN:
      this->publish_state(true);
      break;
    case RainSensorState::DRY:
      this->publish_state(false);
      break;
    case RainSensorState::UNKNOWN:
      // Nothing was read, or the last reads went unanswered; "dry" must mean a reply said so.
      this->invalidate_state();
      break;
  }
}

void IOHomeRainBinarySensor::dump_config() {
  ESP_LOGCONFIG(TAG_RAIN, "IO-Homecontrol Rain Sensor:");
  ESP_LOGCONFIG(TAG_RAIN, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeSunLevelSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    // Every device update reaches here (link health, status, ...); publish only a change.
    if (this->published_once_ && dev.sun_level == this->published_)
      return;
    this->published_once_ = true;
    this->published_ = dev.sun_level;
    // NAN is how an ESPHome sensor says "unknown": nothing read yet, or the last polls went unanswered.
    this->publish_state(dev.sun_level.has_value() ? static_cast<float>(*dev.sun_level) : NAN);
  });
}

void IOHomeSunLevelSensor::dump_config() {
  const char *const TAG = TAG_SUN_LEVEL;  // LOG_SENSOR() logs under a local named TAG
  LOG_SENSOR("", "IO-Homecontrol Sun Level", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeBatteryLevelSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (this->published_once_ && dev.battery_level == this->published_)
      return;
    this->published_once_ = true;
    this->published_ = dev.battery_level;
    this->publish_state(dev.battery_level.has_value() ? static_cast<float>(*dev.battery_level) : NAN);
  });
}

void IOHomeBatteryLevelSensor::dump_config() {
  const char *const TAG = TAG_BATTERY_LEVEL;  // LOG_SENSOR() logs under a local named TAG
  LOG_SENSOR("", "IO-Homecontrol Battery Level", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeBatteryLowBinarySensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) { this->publish_battery_(dev); });
}

void IOHomeBatteryLowBinarySensor::publish_battery_(const IoDevice &dev) {
  switch (dev.battery) {
    case BatteryState::LOW_CHARGE:
      this->publish_state(true);
      return;
    case BatteryState::NORMAL_CHARGE:
    case BatteryState::FULL_CHARGE:
      this->publish_state(false);
      return;
    case BatteryState::NOT_REPORTED:
      break;
  }
  // "off" must mean the device said its battery is fine, so no level means no state.
  this->invalidate_state();
  // last_status != 0: a status was decoded and carried no battery, rather than none read yet.
  if (dev.last_status != 0 && !this->logged_not_reported_) {
    this->logged_not_reported_ = true;
    ESP_LOGI(TAG_BATTERY, "Device %s reports no battery state; it is probably mains powered", this->device_id_.c_str());
  }
}

void IOHomeBatteryLowBinarySensor::dump_config() {
  ESP_LOGCONFIG(TAG_BATTERY, "IO-Homecontrol Battery Low:");
  ESP_LOGCONFIG(TAG_BATTERY, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeBatteryTextSensor::setup() {
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (dev.battery != BatteryState::NOT_REPORTED)
      this->publish_state(battery_state_name(dev.battery));
  });
}

void IOHomeBatteryTextSensor::dump_config() {
  ESP_LOGCONFIG(TAG_BATTERY, "IO-Homecontrol Battery:");
  ESP_LOGCONFIG(TAG_BATTERY, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
