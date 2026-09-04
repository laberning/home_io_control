/// @file platform_hub_controls.cpp
/// @brief Hub-level control entities: arming switches and the pairing-result text sensor.
/// @ingroup hioc_platforms

#include "platform_hub_controls.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

// Each dump_config() keeps its own log tag as a `static` function-local constant: the ESPHome
// LOG_SWITCH / LOG_TEXT_SENSOR macros expand to a bare `TAG` identifier, so one shared file-scope
// constant could not carry the different values. Values are unchanged from the former per-entity
// files. (`static` so clang-tidy's identifier-naming check treats it as a StaticConstant.)

void HubArmingSwitch::setup() {
  if (this->parent_ == nullptr)
    return;
  this->subscribe_armed([this](bool armed) { this->publish_state(armed); });
}

void HubArmingSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->arm(state);
  this->publish_state(state);
}

void IOHomeAcceptForeignPairingSwitch::dump_config() {
  static const char *const TAG = "home_io_control.key_extraction_switch";
  LOG_SWITCH("", "IO-Homecontrol Recover System Key (Key Extraction)", this);
  ESP_LOGCONFIG(TAG, "  Status: hardware-confirmed protocol; not yet confirmed against a third-party hub");
}

void IOHomeRecoverOneWayKeySwitch::dump_config() {
  static const char *const TAG = "home_io_control.oneway_key_switch";
  LOG_SWITCH("", "IO-Homecontrol Recover 1W Controller Key", this);
  ESP_LOGCONFIG(TAG, "  Receive-only: listens for an add-controller broadcast, never transmits");
}

void IOHomePairingResultTextSensor::setup() {
  if (this->parent_ == nullptr)
    return;

  this->parent_->set_pairing_result_callback([this]() { this->on_pairing_result_(); });
}

void IOHomePairingResultTextSensor::dump_config() {
  // [[maybe_unused]]: LOG_TEXT_SENSOR consumes TAG only in the firmware build; the host stub
  // expands it away, and there is no ESP_LOGCONFIG line here to reference it.
  [[maybe_unused]] static const char *const TAG = "home_io_control.pairing_result";
  LOG_TEXT_SENSOR("", "IO-Homecontrol Last Pairing Result", this);
}

void IOHomePairingResultTextSensor::on_pairing_result_() {
  if (this->parent_ == nullptr)
    return;
  this->publish_state(this->parent_->pairing_telemetry().result_sensor_string());
}

}  // namespace home_io_control
}  // namespace esphome
