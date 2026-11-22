#pragma once

/// @file platform_switch.h
/// @brief Binary switch entity for IO‑Homecontrol devices.
/// @ingroup hioc_platforms
///
/// Provides a minimal on/off switch representation. Position < 50 is treated as on.
/// One field report covers it: a Somfy Izymo on/off receiver (type 15, subtype 2) switches on and off
/// and follows a linked remote (issue #57). No capture of a switch is in the corpus.
/// @todo Confirm with a capture of a physical switch: startup state restoration and any
///       vendor-specific position encodings that do not map cleanly to binary on/off semantics.

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
  /// @param dev Updated IoDevice state.
  void on_device_update_(const IoDevice &dev);
};

}  // namespace home_io_control
}  // namespace esphome