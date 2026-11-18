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
  DeviceReadingValues &store = dev.reading_values;
  if (const auto level = decode_status_reading(DeviceReading::HEATING_LEVEL, main_parameter, profile->mp_prefix,
                                               dev.temperature_range_min_c, dev.temperature_range_max_c)) {
    if (reading_wire(dev, DeviceReading::HEATING_LEVEL) != static_cast<int>(*level))
      ESP_LOGI(detail::TAG, "Device %s: heating value 0x%02X", id.c_str(), static_cast<unsigned>(*level));
    store.store(reading_slot(DeviceReading::HEATING_LEVEL), *level);
    return;
  }
  if (profile->path == HeatingControlPath::TEMPERATURE_EXECUTE) {
    if (const auto celsius = decode_status_reading(DeviceReading::STATUS_SETPOINT, main_parameter, profile->mp_prefix,
                                                   dev.temperature_range_min_c, dev.temperature_range_max_c)) {
      // A plain temperature ends an alias the heater reported before.
      store.store(reading_slot(DeviceReading::STATUS_SETPOINT), *celsius);
      store.clear(reading_slot(DeviceReading::HEATING_LEVEL));
      return;
    }
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
  const auto level = decode_status_reading(DeviceReading::HEATING_LEVEL, main_parameter, HEATING_LEVEL_MP_PREFIX,
                                           dev.temperature_range_min_c, dev.temperature_range_max_c);
  if (!level) {
    // Not a level: the stored level is forgotten. The heating_level sensor keeps what it last showed (a text
    // sensor cannot go back to no state).
    ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%02X%02X is not a heating level", id.c_str(),
             main_parameter[0], main_parameter[1]);
    dev.reading_values.clear(reading_slot(DeviceReading::HEATING_LEVEL));
    return;
  }
  if (reading_wire(dev, DeviceReading::HEATING_LEVEL) != static_cast<int>(*level))
    ESP_LOGI(detail::TAG, "Device %s: heating level 0x%02X", id.c_str(), static_cast<unsigned>(*level));
  dev.reading_values.store(reading_slot(DeviceReading::HEATING_LEVEL), *level);
}

void apply_dhw_status_reply(const std::string &id, IoDevice &dev, const IoFrame &frame, bool trust_position) {
  if (!trust_position)
    return;
  DhwOperatingStatus status;
  if (decode_dhw_operating_status(frame, status)) {
    if (dev.dhw.boost != (status.boost ? 1 : 0) || dev.dhw.away != (status.away ? 1 : 0)) {
      ESP_LOGI(detail::TAG, "Device %s: hot water boost %s, away %s (FP15 0x%04X)", id.c_str(),
               status.boost ? "on" : "off", status.away ? "on" : "off", static_cast<unsigned>(status.capabilities));
    }
    dev.dhw.capabilities = status.capabilities;
    dev.dhw.boost = status.boost ? 1 : 0;
    dev.dhw.away = status.away ? 1 : 0;
  }
  float celsius = NAN;
  if (decode_dhw_status_target(frame, celsius)) {
    if (dev.dhw.target_c != celsius)
      ESP_LOGI(detail::TAG, "Device %s: hot water target %.1f C", id.c_str(), static_cast<double>(celsius));
    dev.dhw.target_c = celsius;
    return;
  }
  // An alias or an out-of-range value is not a target temperature: it is logged and the target shows unknown.
  ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%02X%02X is not a hot water target", id.c_str(),
           frame.data[DHW_STATUS_MAIN_PARAMETER_OFFSET], frame.data[DHW_STATUS_MAIN_PARAMETER_OFFSET + 1]);
  dev.dhw.target_c = NAN;
}

}  // namespace home_io_control
}  // namespace esphome
