#pragma once

/// @file platform_entity_base.h
/// @brief Shared device-binding mixins for IO-Homecontrol entity platforms.
/// @ingroup hioc_platforms
///
/// IOHomeCover, IOHomeLight, IOHomeSwitch and IOHomeLock all bind an ESPHome entity to a hub
/// device: the same YAML setters, the same registration ritual in setup() (which delivers only
/// that device's updates to the entity), the same setup gate for a declared kind the entity cannot
/// drive, and the same poll-interval dump_config line. Settings only one platform has stay on that
/// platform and reach the registration through its configuration hook. DeviceBoundEntity centralizes exactly that
/// shared state and wiring. The auto-generated companion diagnostic sensors share a smaller, observe-only binding;
/// DeviceBoundCompanion centralizes that one. A third mixin, HubBoundEntity, holds the single parent pointer that the
/// hub-level control entities (which act on the hub as a whole and have no device to bind to) would otherwise each
/// restate. The mixins hold the ports of hub_ports.h that their entities use, never the hub class itself.
///
/// All three are intentionally NOT ESPHome base classes — they are plain mixins the entities inherit
/// alongside their real ESPHome base (cover::Cover, light::LightOutput, switch_::Switch,
/// lock::Lock, text_sensor::TextSensor, sensor::Sensor). Entity-specific state mapping (cover
/// position/tilt/movement inference, binary on/off decoding, each companion's value rendering)
/// deliberately stays in the entities; only the device-binding plumbing lives here.

#include "hub_ports.h"
#include "proto_device_kinds.h"
#include "status_poll_policy.h"
#include "proto_frame.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace esphome {
namespace home_io_control {

namespace detail {

/// @brief The port of type @p Port that a hub provides, or null when its type does not implement it.
///
/// Lets an entity's `set_parent(hub)` take the ports it uses from whatever the generated code (or a test)
/// passes, without the entity naming the hub class: the real hub implements all three ports, a test double
/// only the ones its test needs.
/// @tparam Port Port interface to take.
/// @tparam Hub Type of the object passed to `set_parent()`.
/// @param hub Object to take the port from.
template<typename Port, typename Hub> [[nodiscard]] Port *port_of(Hub *hub) {
  if constexpr (std::is_convertible_v<Hub *, Port *>) {
    return hub;
  } else {
    return nullptr;
  }
}

}  // namespace detail

/// @brief Mixin holding the hub-device binding shared by all IO-Homecontrol entity platforms.
/// @ingroup hioc_platforms
class DeviceBoundEntity {
 public:
  /// @brief Wire the entity to the hub (called by the generated code).
  /// @details Takes the device port and the heating port from @p hub, whichever it implements.
  /// @tparam Hub Type of the hub; IOHomeControlComponent, or a test double of the ports.
  /// @param hub Hub to take the ports from.
  template<typename Hub> void set_parent(Hub *hub) {
    this->device_port_ = detail::port_of<HubDevicePort>(hub);
    this->heating_port_ = detail::port_of<HubHeatingPort>(hub);
  }
  /// @brief Set the unique IO-homecontrol device ID (from YAML).
  /// @param id Hexadecimal node ID string (e.g., "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }
  /// @brief Set the declared device type (from YAML).
  /// @param type Device type enum.
  void set_device_type(DeviceType type) { this->device_type_ = type; }
  /// @brief Set the declared device subtype (from YAML).
  /// @param subtype Subtype value.
  void set_subtype(uint8_t subtype) { this->subtype_ = subtype; }
  /// @brief Set the declared product model below the subtype (YAML `io_model:`).
  /// @param model Model; the codegen only emits a model the declared (type, subtype) offers.
  void set_device_model(DeviceModel model) { this->model_ = model; }
  /// @brief Mark this device as a low-power / duty-cycled receiver. Directed frames to it then set
  /// CTRL1_LOW_POWER and use the long wake-up preamble; an always-alive device (the default) gets
  /// neither. Radio property of the device, shared by all four platforms.
  /// @param low_power True for a battery/solar or otherwise sleeping receiver; default false.
  void set_low_power(bool low_power) { this->low_power_ = low_power; }
  /// @brief Give this device a system key of its own (`system_key:` in YAML).
  /// @details Every exchange with the device then uses this key instead of the hub's. The key is
  /// validated as 32 hex characters at config time, so a parse failure here leaves the device on the
  /// hub key.
  /// @param key 32 hex characters.
  void set_system_key(const std::string &key) {
    this->has_system_key_ = hex_to_bytes(key, this->system_key_, AES_KEY_SIZE);
  }
  /// @brief Configure bounded follow-up polling while a state change is expected.
  /// @param poll_interval_ms Poll interval in milliseconds; zero keeps the default single settle poll only.
  void set_status_poll_interval(uint32_t poll_interval_ms) { this->status_poll_interval_ms_ = poll_interval_ms; }

 protected:
  /// @brief Perform the shared setup() registration ritual.
  ///
  /// Registers the device with the controller, sets its status-poll interval, subscribes the
  /// entity's update callback, and schedules the delayed initial status request — in the exact
  /// order every entity used before.
  /// @param self The entity itself; used for set_timeout(), since DeviceBoundEntity is not a Component.
  /// @param entity_class Capability class of the registering entity; decides the features of a device
  ///        whose kind the type table does not know.
  /// @param on_update The entity's device-update handler; called only for this entity's device.
  /// @param configure Optional hook that fills the declaration with the entity's own settings (a cover's
  ///        inversion and travel speed, a light's `dimmable`) after the shared fields are set.
  void register_device_binding_(Component *self, DeviceCapabilityClass entity_class,
                                std::function<void(const IoDevice &)> on_update,
                                const std::function<void(DeviceConfig &)> &configure = {}) {
    DeviceConfig config;
    config.type = this->device_type_;
    config.subtype = this->subtype_;
    config.model = this->model_;
    config.entity_class = entity_class;
    config.low_power = this->low_power_;
    config.has_system_key = this->has_system_key_;
    if (this->has_system_key_)
      memcpy(config.system_key, this->system_key_, AES_KEY_SIZE);
    if (configure)
      configure(config);
    this->device_port_->add_device(this->device_id_, config);
    this->device_port_->set_device_status_poll_interval(this->device_id_, this->status_poll_interval_ms_);
    this->device_port_->register_device_callback(
        [this, on_update = std::move(on_update)](const std::string &id, const IoDevice &dev) {
          if (id == this->device_id_)
            on_update(dev);
        });
    // Schedule the delayed initial poll through the public scheduler: Component::set_timeout is
    // protected, and DeviceBoundEntity is a mixin, not a Component subclass, so it cannot call it
    // through `self`. App.scheduler.set_timeout is the public entry point set_timeout wraps.
    App.scheduler.set_timeout(self, "init_status", INITIAL_STATUS_REQUEST_DELAY_MS,
                              [this]() { this->device_port_->queue_request_device_status(this->device_id_); });
  }

  /// @brief What this entity's device can do: the registered device's features, or, before it is
  /// registered, the features its declared kind resolves to.
  /// @param entity_class Capability class of the asking entity.
  [[nodiscard]] DeviceFeatures device_features_(DeviceCapabilityClass entity_class) const {
    if (this->device_port_ != nullptr) {
      if (const IoDevice *dev = this->device_port_->get_device(this->device_id_))
        return dev->features;
    }
    IoDevice declared{};
    declared.type = this->device_type_;
    declared.subtype = this->subtype_;
    declared.model = this->model_;
    declared.entity_class = entity_class;
    resolve_device_features(declared);
    return declared.features;
  }

  /// @brief Setup gate shared by the entities that drive one family of devices: logs why a declared
  /// device kind can not be driven by this entity.
  ///
  /// A device declared with no type passes: discovery may still resolve it, as on the other platforms.
  /// The caller marks the entity failed when this returns true (Component::mark_failed() is not
  /// reachable from a mixin) and returns before registering the binding, so a misconfigured entity never
  /// injects a device into the hub registry.
  /// @param tag Log tag of the calling entity.
  /// @param drivable Whether the declared kind is one the entity can drive; ignored when no type is declared.
  /// @param hint Clause completing "Device X is declared as Y, <hint> — disabling it".
  /// @return True when a declared kind is not drivable and the line was logged.
  [[nodiscard]] bool reject_undrivable_kind_(const char *tag, bool drivable, const char *hint) const {
    if (this->device_type_ == DeviceType::UNKNOWN || drivable)
      return false;
    ESP_LOGE(tag, "Device %s is declared as %s, %s — disabling it", this->device_id_.c_str(),
             format_device_kind(this->device_type_, this->subtype_).c_str(), hint);
    return true;
  }

  /// @brief Emit the shared two-branch poll-interval line for dump_config().
  /// @param tag Log tag of the calling entity.
  /// @param resolve_interval For an entity with a fixed cadence of its own, the rule that turns the YAML
  ///        interval (zero when unset) into the interval it polls at, e.g. heater_poll_interval_ms(); null
  ///        (the default) prints the YAML interval as set.
  void log_poll_interval_config_(const char *tag, uint32_t (*resolve_interval)(uint32_t) = nullptr) const {
    const uint32_t interval_ms =
        resolve_interval != nullptr ? resolve_interval(this->status_poll_interval_ms_) : this->status_poll_interval_ms_;
    if (interval_ms == 0) {
      ESP_LOGCONFIG(tag, "  Status Poll Interval: device-hinted settle polling (no fixed interval)");
    } else {
      ESP_LOGCONFIG(tag, "  Status Poll Interval: %" PRIu32 " ms", interval_ms);
    }
  }

  HubDevicePort *device_port_{nullptr};
  HubHeatingPort *heating_port_{nullptr};  ///< Null unless the hub passed to set_parent() implements it.
  std::string device_id_;
  DeviceType device_type_{DeviceType::UNKNOWN};
  uint8_t subtype_{0};
  DeviceModel model_{DeviceModel::NONE};
  uint32_t status_poll_interval_ms_{0};
  bool low_power_{false};
  bool has_system_key_{false};
  uint8_t system_key_[AES_KEY_SIZE]{};  ///< Key material: never logged.
};

/// @brief Mixin holding the parent + device-id binding shared by per-device entities that are
/// not full entity platforms: the auto-generated companion diagnostic sensors (device name,
/// active issue, RSSI, last contact, exchange failures, last commanded by, last command source)
/// and the per-device auxiliary control entities (cover favorite/vent buttons, cover silent
/// switch).
/// @ingroup hioc_platforms
///
/// These differ from the main entity platforms (DeviceBoundEntity above) in that they do not own
/// the device: they never call add_device() or configure polling. The companion sensors use
/// register_companion_binding_() to subscribe to update notifications and republish their one
/// value; the auxiliary control entities take only the parent + device-id pair and act on their
/// already-registered device on demand, so register_companion_binding_() stays opt-in. Each class
/// keeps its own value rendering / action logic (including whether a given device state is
/// publishable at all).
class DeviceBoundCompanion {
 public:
  /// @brief Wire the entity to the hub (called by the generated code).
  /// @details Takes the device port and the heating port from @p hub, whichever it implements.
  /// @tparam Hub Type of the hub; IOHomeControlComponent, or a test double of the ports.
  /// @param hub Hub to take the ports from.
  template<typename Hub> void set_parent(Hub *hub) {
    this->device_port_ = detail::port_of<HubDevicePort>(hub);
    this->heating_port_ = detail::port_of<HubHeatingPort>(hub);
  }
  /// @brief Set the device ID whose state this companion sensor exposes.
  /// @param id Hexadecimal node ID string (for example "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }

 protected:
  /// @brief Shared setup() body: subscribe to this device's updates and publish the initial state.
  ///
  /// No-op when no parent is wired (codegen always wires one before setup() runs).
  /// @param publish Renders and publishes the sensor's value from a device record — or skips
  /// publishing when the record has no meaningful value yet. Runs once immediately when the
  /// device is already registered, then again on every update notification for this device.
  void register_companion_binding_(const std::function<void(const IoDevice &)> &publish) {
    if (this->device_port_ == nullptr)
      return;

    this->device_port_->register_device_callback([this, publish](const std::string &id, const IoDevice &dev) {
      if (id == this->device_id_)
        publish(dev);
    });

    if (const auto *dev = this->device_port_->get_device(this->device_id_); dev != nullptr)
      publish(*dev);
  }

  HubDevicePort *device_port_{nullptr};
  HubHeatingPort *heating_port_{nullptr};  ///< Null unless the hub passed to set_parent() implements it.
  std::string device_id_;
};

/// @brief Mixin for entities bound to the hub itself rather than to one device.
/// @ingroup hioc_platforms
///
/// The hub-level control entities (discover button, scan-paired-devices button, arming switches,
/// firmware-update button, 1W command/enroll buttons, pairing-result and 1W-last-command text
/// sensors) need only the control
/// port — they act on the hub as a whole and have no device to bind to. This holds that one
/// setter and one member so each entity does not restate it. The tuning number/select entities
/// are deliberately not migrated: they take the parent by constructor injection and keep
/// hub_core.h out of their header via a forward declaration.
///
/// Why every one of these is created from the `home_io_control:` block (or a dedicated `button:`
/// entry) and never from a device-bound `switch:`/`button:` platform entry: a user-declared
/// device-bound entry would have to dispatch on the presence of a key (`io_device_id`,
/// `commands:`, `enrollment:`, …) to decide what the entity *is*, so an entry that merely omitted
/// `io_device_id` by mistake could be misread as one of these security-sensitive hub entities
/// instead of failing validation. Creating them from the hub block removes that failure mode:
/// there is no shared schema for a device-bound entity and a hub entity to be confused under.
/// This is the shared reason behind all of them; each entity's own doc adds only what is
/// specific to it (the ADR 0021 permission-vs-arming argument, the ADR 0026 physical-interlock
/// argument, the "2W extraction and 1W adoption are deliberately independent" note, and so on).
class HubBoundEntity {
 public:
  /// @brief Wire the entity to the hub (called by the generated code).
  /// @details Takes the control port from @p hub when it implements it.
  /// @tparam Hub Type of the hub; IOHomeControlComponent, or a test double of the control port.
  /// @param hub Hub to take the port from.
  template<typename Hub> void set_parent(Hub *hub) { this->control_port_ = detail::port_of<HubControlPort>(hub); }

 protected:
  HubControlPort *control_port_{nullptr};
};

/// @brief Mixin for the hub-level entities that additionally scope themselves to one
/// `oneway_controllers:` identity: the 1W command buttons, the 1W enrollment button, and the
/// per-identity "Last 1W Command" text sensor.
/// @ingroup hioc_platforms
///
/// They act on the hub, not on a paired device, so they are HubBoundEntity plus this one handle.
/// The identity is a `oneway_controllers:` block entry, not an address in the hub's device table.
class OneWayControllerBound : public HubBoundEntity {
 public:
  /// @brief Set the controller identity this entity acts as / reports on.
  /// @param id Handle from the `oneway_controllers:` block.
  void set_controller_id(const std::string &id) { this->controller_id_ = id; }

 protected:
  std::string controller_id_;
};

}  // namespace home_io_control
}  // namespace esphome
