/// @file platform_oneway_enroll_button.cpp
/// @brief Generated 1W enrollment button.
/// @ingroup hioc_platforms

#include "platform_oneway_enroll_button.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

void IOHomeOneWayEnrollButton::dump_config() {
  ESP_LOGCONFIG("home_io_control.button", "IO-Homecontrol 1W Enroll Button");
  ESP_LOGCONFIG("home_io_control.button", "  Controller: %s", this->controller_id_.c_str());
}

}  // namespace home_io_control
}  // namespace esphome
