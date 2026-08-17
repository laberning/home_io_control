/// @file platform_recover_oneway_key_switch.cpp
/// @brief Hub-level "Recover 1W Controller Key" switch entity (key adoption, 1W).
/// @ingroup hioc_platforms

#include "platform_recover_oneway_key_switch.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.oneway_key_switch";

void IOHomeRecoverOneWayKeySwitch::setup() {
  if (this->parent_ == nullptr)
    return;
  this->parent_->set_oneway_key_adoption_armed_callback([this](bool armed) { this->publish_state(armed); });
}

void IOHomeRecoverOneWayKeySwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_oneway_key_adoption_armed(state);
  this->publish_state(state);
}

void IOHomeRecoverOneWayKeySwitch::dump_config() {
  LOG_SWITCH("", "IO-Homecontrol Recover 1W Controller Key", this);
  ESP_LOGCONFIG(TAG, "  Receive-only: listens for an add-controller broadcast, never transmits");
}

}  // namespace home_io_control
}  // namespace esphome
