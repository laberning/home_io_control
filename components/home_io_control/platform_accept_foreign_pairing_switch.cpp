/// @file platform_accept_foreign_pairing_switch.cpp
/// @brief Hub-level "Accept Foreign Pairing (Key Extraction)" switch entity.
/// @ingroup hioc_platforms

#include "platform_accept_foreign_pairing_switch.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.key_extraction_switch";

void IOHomeAcceptForeignPairingSwitch::setup() {
  if (this->parent_ == nullptr)
    return;
  this->parent_->set_key_extraction_armed_callback([this](bool armed) { this->publish_state(armed); });
}

void IOHomeAcceptForeignPairingSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_key_extraction_armed(state);
  this->publish_state(state);
}

void IOHomeAcceptForeignPairingSwitch::dump_config() {
  LOG_SWITCH("", "IO-Homecontrol Accept Foreign Pairing (Key Extraction)", this);
  ESP_LOGCONFIG(TAG, "  Status: hardware-confirmed protocol; not yet confirmed against a third-party hub");
}

}  // namespace home_io_control
}  // namespace esphome
