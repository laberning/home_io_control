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
#include "proto_dhw.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint16_t MAIN_PARAMETER_LOW_BYTE_MASK = 0xFF;  ///< Low byte of a 16-bit main parameter.

}  // namespace

void apply_heating_main_parameter(const std::string &id, IoDevice &dev, uint16_t current) {
  const HeatingProfile *profile = find_heating_profile(dev);
  if (profile == nullptr || profile->path == HeatingControlPath::PRIVATE_REGISTERS)
    return;
  dev.heating.main_parameter = current;
  if ((current >> BITS_PER_BYTE) == profile->mp_prefix) {
    const auto wire = static_cast<int16_t>(current & MAIN_PARAMETER_LOW_BYTE_MASK);
    if (dev.heating.level != wire)
      ESP_LOGI(detail::TAG, "Device %s: heating value 0x%02X", id.c_str(), static_cast<unsigned>(wire));
    dev.heating.level = wire;
    return;
  }
  float celsius = 0.0F;
  if (profile->path == HeatingControlPath::TEMPERATURE_EXECUTE &&
      decode_temperature_main_parameter(current, dev.temperature_range_min_c, dev.temperature_range_max_c, celsius)) {
    dev.heating.setpoint_c = celsius;
    dev.heating.level = -1;
    return;
  }
  ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%04X is not a heating value", id.c_str(), current);
}

void apply_register_heater_status_reply(const std::string &id, IoDevice &dev, const IoFrame &frame,
                                        bool trust_position) {
  if (!trust_position)
    return;
  // The reply to the main-parameter status read (create_status_mp_fp_read()), which carries the heating level
  // at the offset every such reply holds its main parameter.
  const auto main_parameter = static_cast<uint16_t>((frame.data[STATUS_MP_FP_MAIN_PARAMETER_OFFSET] << BITS_PER_BYTE) |
                                                    frame.data[STATUS_MP_FP_MAIN_PARAMETER_OFFSET + 1]);
  uint8_t level = 0;
  if (!decode_heating_level(main_parameter, level)) {
    // Not a level: the stored level is forgotten. The heating_level sensor keeps what it last showed (a text
    // sensor cannot go back to no state).
    ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%04X is not a heating level", id.c_str(), main_parameter);
    dev.heating.level = -1;
    return;
  }
  const auto stored = static_cast<int16_t>(level);
  if (dev.heating.level != stored)
    ESP_LOGI(detail::TAG, "Device %s: heating level 0x%02X", id.c_str(), static_cast<unsigned>(level));
  dev.heating.level = stored;
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
  if (decode_dhw_status_target(frame, dev.temperature_range_min_c, dev.temperature_range_max_c, celsius)) {
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
