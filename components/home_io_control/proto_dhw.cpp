/// @file proto_dhw.cpp
/// @brief Wire facts of a domestic hot water heater that are not registers: its operating-mode word and the names of
/// its modes and heating sources.
/// @ingroup hioc_protocol

#include "proto_dhw.h"

namespace esphome {
namespace home_io_control {

const char *dhw_heating_source_name(int heat_pump, int element) {
  if (heat_pump < 0 || element < 0)
    return nullptr;
  if (heat_pump != 0)
    return element != 0 ? "heat pump and electric backup" : "heat pump";
  return element != 0 ? "electric backup" : "idle";
}

const char *dhw_mode_name(uint8_t value) {
  switch (static_cast<DhwMode>(value)) {
    case DhwMode::AUTO:
      return "auto";
    case DhwMode::MANUAL_ECO_ON:
      return "manual eco on";
    case DhwMode::MANUAL_ECO_OFF:
      return "manual eco off";
  }
  return nullptr;
}

}  // namespace home_io_control
}  // namespace esphome
