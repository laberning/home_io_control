#pragma once

/// @file platform_fan.h
/// @brief Experimental fan entity for IO-Homecontrol exterior heating (a heat level, 0-100 %).
/// @ingroup hioc_platforms
///
/// Exterior heating (node type 21) takes a heat level in the position field rather than a
/// temperature, so it is a fan entity: on/off plus a 0-100 % speed that is the heat level. The
/// mapping is the dimmable light's: heat `p` % is the IO position `100 - p` (MP = position x 0x200,
/// so 100 % heat is `0x0000` and off is `0xC800`; KLF 200 API specification, Table 277). Every
/// exterior heater is sent the main parameter only (no functional-parameter block); subtype 58 is
/// on/off, so it advertises no speed.
///
/// A command shows the commanded on/off and level until the device's status reply (the settle poll
/// after the command, then the regular poll) replaces it; a reply is always the observation that
/// wins. Turning on without a level uses the last non-zero level, else full heat.
///
/// @todo Experimental: no exterior heater has been exercised on hardware. Confirm the position
///       mapping and the status read with a capture or a reporter's heater.

#include "esphome/components/fan/fan.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Fan entity for IO-Homecontrol exterior heating.
/// @ingroup hioc_platforms
///
/// The device-binding setters, setup() registration ritual and poll-interval dump line come from
/// DeviceBoundEntity; the heat-level command and status decoding live here.
class IOHomeFan : public fan::Fan, public Component, public DeviceBoundEntity {
 public:
  /// @brief Register the entity with the shared hub and schedule the first status poll.
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Traits: a 100-step speed (the heat level) unless the device takes the main parameter
  /// only (subtype 58, on/off). No oscillation, no direction.
  fan::FanTraits get_traits() override;

 protected:
  /// @brief Apply a Home Assistant fan request: a heat level, or on/off.
  /// @param call The requested changes.
  void control(const fan::FanCall &call) override;

  /// @brief Show a settled device status: heat level = 100 - position.
  /// @param id Device ID.
  /// @param dev Updated device state.
  void on_device_update_(const std::string &id, const IoDevice &dev);
};

}  // namespace home_io_control
}  // namespace esphome
