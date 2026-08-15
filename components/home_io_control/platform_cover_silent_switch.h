#pragma once

/// @file platform_cover_silent_switch.h
/// @brief Generated silent-operation toggle for cover entities that declare `silent:`.
/// @ingroup hioc_platforms

#include <string>

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Switch entity that selects a cover's travel profile at runtime.
/// @ingroup hioc_platforms
///
/// ON sends position commands with the manufacturer app's "silent operation" extended block, which
/// makes the motor travel more slowly; OFF sends the normal payload. Only generated when the cover
/// declares `silent:` in YAML, so a config that never mentions it gains no entity — the YAML value
/// is the boot state and this switch moves it afterwards.
///
/// Device-bound like IOHomeCoverFavoriteButton rather than hub-level like
/// IOHomeAcceptForeignPairingSwitch: the setting is per actuator. Nothing on the wire reports a
/// device's current profile, so this reflects only what this hub has been told to send — there is
/// no readback to reconcile against, and none is faked.
class IOHomeCoverSilentSwitch : public switch_::Switch, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }

  /// @brief Set the device ID whose travel profile this switch selects.
  /// @param id Hexadecimal node ID string (e.g. "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }

  /// @brief Publish the boot state declared in YAML so Home Assistant starts in sync.
  void setup() override;

  /// @brief Dump switch configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Apply the requested travel profile to the bound device.
  /// @param state True for silent (slower) operation.
  void write_state(bool state) override;

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
};

}  // namespace home_io_control
}  // namespace esphome
