#pragma once

/// @file platform_switch.h
/// @brief Experimental binary switch entity for IO‑Homecontrol devices.
/// @ingroup hioc_platforms
///
/// Provides a minimal on/off switch representation. Position < 50 is treated as on.
/// This platform is experimental and not yet validated on real hardware.
/// @todo Validate end-to-end behavior with physical IO-Homecontrol switch devices,
///       including passive state updates, startup state restoration, and any vendor-specific
///       position encodings that do not map cleanly to binary on/off semantics.
///
/// @warning This platform has not been tested with physical IO‑Homecontrol switch devices.
///          Behavior is inferred from the protocol specification and may be incomplete.
///          Use with caution and verify with actual hardware before production deployment.

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Binary switch entity for IO‑Homecontrol on/off devices.
/// @ingroup hioc_platforms
///
/// The device-binding setters, setup() registration ritual and poll-interval dump line come
/// from DeviceBoundEntity; only the on/off command and status decoding live here.
class IOHomeSwitch : public switch_::Switch, public Component, public DeviceBoundEntity {
 public:
  /// @brief Initialize the switch entity.
  void setup() override;
  /// @brief Dump configuration to log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Write state change to the device.
  /// @param state Desired on/off state.
  void write_state(bool state) override;
  /// @brief Callback when device state changes (e.g., from a remote).
  /// @param id Device ID.
  /// @param dev Updated IoDevice state.
  void on_device_update_(const std::string &id, const IoDevice &dev);
};

}  // namespace home_io_control
}  // namespace esphome