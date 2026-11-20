/// @file heater_controller_status.cpp
/// @brief Decoding of a heater's and a water heater's status answer into the device record.
/// @ingroup hioc_hub
///
/// Stateless functions: the hub's status decoder calls them with the device record and the part of the
/// reply it has already located (heater_controller.h).

#include "entity_helpers.h"
#include "heater_controller.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace esphome {
namespace home_io_control {

void apply_heating_main_parameter(const std::string &id, IoDevice &dev, std::span<const uint8_t> main_parameter) {
  const HeatingProfile *profile = find_heating_profile(dev);
  if (profile == nullptr || profile->path == HeatingControlPath::PRIVATE_REGISTERS)
    return;
  if (profile->path == HeatingControlPath::HEATING_LEVEL && !profile->status_reports_level) {
    // A heater whose status carries no level (a pilot-wire interface): whatever its main parameter holds is not
    // shown as one, and the card keeps the order last sent.
    ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%02X%02X ignored, this heater reports no level",
             id.c_str(), main_parameter[0], main_parameter[1]);
    return;
  }
  if (const auto level =
          store_status_main_parameter(dev, DeviceReading::HEATING_LEVEL, main_parameter, profile->mp_prefix);
      level.value) {
    if (level.changed)
      ESP_LOGI(detail::TAG, "Device %s: heating value 0x%02X", id.c_str(), static_cast<unsigned>(*level.value));
    return;
  }
  if (profile->path == HeatingControlPath::TEMPERATURE_EXECUTE &&
      store_status_main_parameter(dev, DeviceReading::STATUS_SETPOINT, main_parameter, profile->mp_prefix).value) {
    // A plain temperature ends an alias the heater reported before.
    clear_reading(dev, DeviceReading::HEATING_LEVEL);
    return;
  }
  ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%02X%02X is not a heating value", id.c_str(),
           main_parameter[0], main_parameter[1]);
}

void apply_register_heater_status_reply(const std::string &id, IoDevice &dev, const IoFrame &frame,
                                        bool trust_position) {
  if (!trust_position)
    return;
  // The reply to the main-parameter status read (create_status_mp_fp_read()), which carries the heating level
  // at the offset every such reply holds its main parameter.
  const std::span<const uint8_t> main_parameter(frame.data + STATUS_MP_FP_MAIN_PARAMETER_OFFSET,
                                                STATUS_MAIN_PARAMETER_SIZE);
  const StatusStore level =
      store_status_main_parameter(dev, DeviceReading::HEATING_LEVEL, main_parameter, HEATING_LEVEL_MP_PREFIX);
  if (!level.value) {
    // Not a level: the stored level is forgotten. The heating_level sensor keeps what it last showed (a text
    // sensor cannot go back to no state).
    ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%02X%02X is not a heating level", id.c_str(),
             main_parameter[0], main_parameter[1]);
    clear_reading(dev, DeviceReading::HEATING_LEVEL);
    return;
  }
  if (level.changed)
    ESP_LOGI(detail::TAG, "Device %s: heating level 0x%02X", id.c_str(), static_cast<unsigned>(*level.value));
}

void apply_dhw_status_reply(const std::string &id, IoDevice &dev, const IoFrame &frame, bool trust_position) {
  if (!trust_position)
    return;
  const std::span<const uint8_t> main_parameter(frame.data + STATUS_MP_FP_MAIN_PARAMETER_OFFSET,
                                                STATUS_MAIN_PARAMETER_SIZE);
  // The operating-mode words (FP15, FP16) come with the main parameter in the poll's status read; a reply without them
  // (the answer to a plain status read) leaves boost and the capabilities as they were. Away is read from FP16 too,
  // or from the main parameter where the heater's row reads it there (an Atlantic Explorer V4).
  const int boost_before = reading_wire(dev, DeviceReading::HOT_WATER_BOOST);
  const int away_before = reading_wire(dev, DeviceReading::HOT_WATER_AWAY);
  const size_t parameters = store_status_parameter_readings(dev, frame);
  const bool away_in_main_parameter =
      store_status_main_parameter(dev, DeviceReading::HOT_WATER_AWAY, main_parameter, 0).value.has_value();
  if ((parameters > 0 || away_in_main_parameter) &&
      (reading_wire(dev, DeviceReading::HOT_WATER_BOOST) != boost_before ||
       reading_wire(dev, DeviceReading::HOT_WATER_AWAY) != away_before)) {
    ESP_LOGI(detail::TAG, "Device %s: hot water boost %s, away %s (FP15 0x%04X)", id.c_str(),
             reading_wire(dev, DeviceReading::HOT_WATER_BOOST) == 1 ? "on" : "off",
             reading_wire(dev, DeviceReading::HOT_WATER_AWAY) == 1 ? "on" : "off",
             static_cast<unsigned>(std::max(0, reading_wire(dev, DeviceReading::HOT_WATER_CAPABILITIES))));
  }

  // A water heater's main parameter is its target, over the range of the heater's model (the row its capabilities
  // select): no level prefix applies.
  if (const StatusStore target = store_status_main_parameter(dev, DeviceReading::HOT_WATER_TARGET, main_parameter, 0);
      target.value) {
    if (target.changed)
      ESP_LOGI(detail::TAG, "Device %s: hot water target %.1f C", id.c_str(), static_cast<double>(*target.value));
    return;
  }
  // An alias or an out-of-range value is not a target temperature: it is logged and the target shows unknown.
  ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%02X%02X is not a hot water target", id.c_str(),
           main_parameter[0], main_parameter[1]);
  clear_reading(dev, DeviceReading::HOT_WATER_TARGET);
}

}  // namespace home_io_control
}  // namespace esphome
