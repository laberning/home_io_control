/// @file proto_dhw.cpp
/// @brief Wire facts of a domestic hot water heater that are not registers: its operating-mode word and the names of
/// its modes and heating sources.
/// @ingroup hioc_protocol

#include "proto_dhw.h"

#include <iterator>

namespace esphome {
namespace home_io_control {

namespace {

/// One row per DhwOperatingFeature, in its order.
constexpr DhwOperatingFeatureWire DHW_OPERATING_FEATURES[] = {
    {DHW_CAPABILITY_BOOST, DHW_OPERATING_MODE_BOOST_ON, DeviceRegister::DHW_BOOST_DURATION, DHW_BOOST_START_DURATION},
    {DHW_CAPABILITY_AWAY, DHW_OPERATING_MODE_AWAY_ON, DeviceRegister::DHW_AWAY_DURATION, DHW_AWAY_UNTIL_TURNED_OFF},
};
static_assert(std::size(DHW_OPERATING_FEATURES) == static_cast<size_t>(DhwOperatingFeature::AWAY) + 1,
              "one row per DhwOperatingFeature");

}  // namespace

const DhwOperatingFeatureWire &dhw_operating_feature_wire(DhwOperatingFeature which) {
  return DHW_OPERATING_FEATURES[static_cast<size_t>(which)];
}

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
