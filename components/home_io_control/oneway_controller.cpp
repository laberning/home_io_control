/// @file oneway_controller.cpp
/// @brief Controller identities for the one-way (1W) protocol.
/// @ingroup hioc_protocol

#include "oneway_controller.h"

namespace esphome {
namespace home_io_control {

const char *oneway_button_action_name(OneWayButtonAction action) {
  switch (action) {
    case OneWayButtonAction::OPEN:
      return "OPEN";
    case OneWayButtonAction::CLOSE:
      return "CLOSE";
    case OneWayButtonAction::VENT:
      return "VENT";
    case OneWayButtonAction::FORCE_OPEN:
      return "FORCE_OPEN";
    case OneWayButtonAction::FAVORITE:
      return "FAVORITE";
    case OneWayButtonAction::STOP:
    default:
      return "STOP";
  }
}

}  // namespace home_io_control
}  // namespace esphome
