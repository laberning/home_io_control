/// @file platform_cover_controls.cpp
/// @brief Generated per-cover auxiliary entities: command button, silent-operation toggle and travel speed select.
/// @ingroup hioc_platforms

#include "platform_cover_controls.h"

#include "esphome/core/log.h"
#include "proto_device_kinds.h"

namespace esphome {
namespace home_io_control {

// One tag for the command button, shared by every command (favorite, vent, ...): a user greps for
// the entity by name, not by log tag, so the tag need not name the command.
static const char *const TAG_COMMAND_BUTTON = "home_io_control.cover_command_button";
static const char *const TAG_SILENT = "home_io_control.cover.silent";
static const char *const TAG_TRAVEL_SPEED = "home_io_control.cover.travel_speed";
static const char *const TAG_MANUAL_MODE = "home_io_control.cover.manual_mode";

void IOHomeCoverCommandButton::press_action() {
  if (this->device_port_ == nullptr) {
    ESP_LOGW(TAG_COMMAND_BUTTON, "Ignoring %s press because the parent hub is not configured",
             cover_command_name(this->command_));
    return;
  }

  if (this->device_port_->get_device(this->device_id_) == nullptr) {
    ESP_LOGW(TAG_COMMAND_BUTTON, "Ignoring %s press for %s because the device is not registered yet",
             cover_command_name(this->command_), this->device_id_.c_str());
    return;
  }

  this->device_port_->queue_device_command(this->device_id_, this->command_);
}

void IOHomeCoverCommandButton::dump_config() {
  ESP_LOGCONFIG(TAG_COMMAND_BUTTON, "IO-Homecontrol %s Position Button", cover_command_name(this->command_));
  ESP_LOGCONFIG(TAG_COMMAND_BUTTON, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeCoverSilentSwitch::setup() {
  // The YAML value was already applied to the device at registration; mirror it outward so Home
  // Assistant does not start out disagreeing with what the hub will actually send.
  if (this->device_port_ == nullptr)
    return;
  if (const IoDevice *dev = this->device_port_->get_device(this->device_id_); dev != nullptr)
    this->publish_state(dev->travel_speed == TravelSpeed::SILENT);
}

void IOHomeCoverSilentSwitch::write_state(bool state) {
  if (this->device_port_ == nullptr)
    return;
  this->device_port_->set_device_travel_speed(this->device_id_, state ? TravelSpeed::SILENT : TravelSpeed::FAST);
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
  if (this->device_port_ == nullptr)
    return;
  if (const IoDevice *dev = this->device_port_->get_device(this->device_id_); dev != nullptr)
    this->publish_state(travel_speed_name(dev->travel_speed));
}

void IOHomeCoverTravelSpeedSelect::control(const std::string &value) {
  if (this->device_port_ == nullptr)
    return;
  TravelSpeed speed = TravelSpeed::FAST;
  if (!parse_travel_speed(value, speed)) {
    ESP_LOGW(TAG_TRAVEL_SPEED, "Device %s: ignoring unknown travel speed '%s'", this->device_id_.c_str(),
             value.c_str());
    return;
  }
  this->device_port_->set_device_travel_speed(this->device_id_, speed);
  this->publish_state(value);
  ESP_LOGI(TAG_TRAVEL_SPEED, "Device %s: travel speed %s", this->device_id_.c_str(), travel_speed_name(speed));
}

void IOHomeCoverTravelSpeedSelect::dump_config() {
  ESP_LOGCONFIG(TAG_TRAVEL_SPEED, "IO-Homecontrol Travel Speed:");
  ESP_LOGCONFIG(TAG_TRAVEL_SPEED, "  Device ID: %s", this->device_id_.c_str());
}

void IOHomeCoverManualModeBinarySensor::setup() {
  // Fed by the answers to set_position_automatic and by heard flips of a linked A/M switch; the sensor sends nothing.
  this->register_companion_binding_([this](const IoDevice &dev) {
    if (dev.manual_mode != ManualMode::UNKNOWN)
      this->publish_state(dev.manual_mode == ManualMode::MANUAL);
  });
}

void IOHomeCoverManualModeBinarySensor::dump_config() {
  ESP_LOGCONFIG(TAG_MANUAL_MODE, "IO-Homecontrol Manual Mode:");
  ESP_LOGCONFIG(TAG_MANUAL_MODE, "  Device ID: %s", this->device_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
