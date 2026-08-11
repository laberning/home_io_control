/// @file platform_oneway_command_button.cpp
/// @brief Generated 1W command button.
/// @ingroup hioc_platforms

#include "platform_oneway_command_button.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

void IOHomeOneWayCommandButton::dump_config() {
  ESP_LOGCONFIG("home_io_control.button", "IO-Homecontrol 1W Command Button");
  ESP_LOGCONFIG("home_io_control.button", "  Controller: %s", this->controller_id_.c_str());
  ESP_LOGCONFIG("home_io_control.button", "  Action: %s", oneway_button_action_name(this->action_));
}

}  // namespace home_io_control
}  // namespace esphome
