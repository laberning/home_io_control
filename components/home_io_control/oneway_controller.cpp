/// @file oneway_controller.cpp
/// @brief Controller identities for the one-way (1W) protocol.
/// @ingroup hioc_protocol

#include "oneway_controller.h"

namespace esphome {
namespace home_io_control {

const char *oneway_power_class_name(OneWayPowerClass power_class) {
  switch (power_class) {
    case OneWayPowerClass::ALWAYS_ALIVE:
      return "always-alive";
    case OneWayPowerClass::LOW_POWER:
      return "low-power";
    case OneWayPowerClass::LEGACY_LONG:
    default:
      return "legacy long";
  }
}

const char *oneway_button_action_name(OneWayButtonAction action) {
  switch (action) {
    case OneWayButtonAction::OPEN:
      return "OPEN";
    case OneWayButtonAction::CLOSE:
      return "CLOSE";
    case OneWayButtonAction::VENT:
      return "VENT";
    case OneWayButtonAction::FAVORITE:
      return "FAVORITE";
    case OneWayButtonAction::STOP:
    default:
      return "STOP";
  }
}

}  // namespace home_io_control
}  // namespace esphome
