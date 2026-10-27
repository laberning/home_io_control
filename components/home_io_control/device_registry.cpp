/// @file device_registry.cpp
/// @brief DeviceRegistry implementation.
/// @ingroup hioc_hub

#include "device_registry.h"

#include "proto_frame.h"
#include "proto_sizes.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace home_io_control {

static constexpr const char *TAG = "home_io_control";

void DeviceRegistry::add(const std::string &device_id) { add(device_id, DeviceConfig{}); }

void DeviceRegistry::add(const std::string &device_id, const DeviceConfig &cfg) {
  if (devices_.count(device_id) != 0)
    return;
  IoDevice dev{};
  if (!hex_to_bytes(device_id, dev.node_id, NODE_ID_SIZE)) {
    ESP_LOGW(TAG, "Ignoring invalid device ID %s", device_id.c_str());
    return;
  }
  dev.type = cfg.type;
  dev.subtype = cfg.subtype;
  dev.entity_class = cfg.entity_class;
  dev.invert_override = cfg.invert_override;
  dev.dimmable = cfg.dimmable;
  dev.optimistic_state = cfg.optimistic_state;
  dev.silent = cfg.silent;
  // A `true` is a declaration wherever it comes from, so the config dump never calls it the band's default.
  dev.low_power_set = cfg.low_power_set || cfg.low_power;
  dev.low_power = cfg.low_power;
  this->apply_low_power_default_(dev);
  dev.battery_entities = cfg.battery_entities;
  dev.has_system_key = cfg.has_system_key;
  if (cfg.has_system_key)
    memcpy(dev.system_key, cfg.system_key, AES_KEY_SIZE);
  resolve_device_features(dev);
  devices_[device_id] = dev;
}

void DeviceRegistry::put(const std::string &device_id, IoDevice device) {
  const auto existing = devices_.find(device_id);
  if (existing != devices_.end()) {
    device.entity_class = existing->second.entity_class;
    device.invert_override = existing->second.invert_override;
    device.dimmable = existing->second.dimmable;
  }
  resolve_device_features(device);
  this->apply_low_power_default_(device);
  devices_[device_id] = device;
}

void DeviceRegistry::set_low_power_default(bool low_power) {
  default_low_power_ = low_power;
  for (auto &entry : devices_)
    this->apply_low_power_default_(entry.second);
}

void DeviceRegistry::apply_low_power_default_(IoDevice &device) const {
  // A `true` from anywhere (YAML, a discovery that reported the class) stands; only a flag nobody set
  // to true and nobody declared takes the band's default.
  if (!device.low_power_set && !device.low_power)
    device.low_power = default_low_power_;
}

IoDevice *DeviceRegistry::get(const std::string &device_id) {
  auto it = devices_.find(device_id);
  return (it != devices_.end()) ? &it->second : nullptr;
}

IoDevice *DeviceRegistry::get_declared(const std::string &device_id) {
  auto it = devices_.find(device_id);
  return (it != devices_.end() && !it->second.registered_by_pairing) ? &it->second : nullptr;
}

bool DeviceRegistry::remove_registered_by_pairing(const std::string &device_id) {
  auto it = devices_.find(device_id);
  if (it == devices_.end() || !it->second.registered_by_pairing)
    return false;
  devices_.erase(it);
  return true;
}

void DeviceRegistry::set_silent(const std::string &device_id, bool silent) {
  if (IoDevice *dev = this->get(device_id))
    dev->silent = silent;
}

void DeviceRegistry::subscribe(DeviceUpdateCallback cb) { callbacks_.push_back(std::move(cb)); }

void DeviceRegistry::notify(const std::string &device_id) {
  auto it = devices_.find(device_id);
  if (it == devices_.end())
    return;
  for (auto &cb : callbacks_)
    cb(device_id, it->second);
}

void DeviceRegistry::add_linked_remote(const std::string &remote_id, const std::string &device_id) {
  linked_remotes_[remote_id].push_back(device_id);
}

const std::vector<std::string> *DeviceRegistry::linked_devices(const std::string &remote_id) const {
  auto it = linked_remotes_.find(remote_id);
  return (it != linked_remotes_.end()) ? &it->second : nullptr;
}

void DeviceRegistry::add_linked_remote_class(DeviceType type, const std::string &device_id) {
  linked_remote_classes_[type].push_back(device_id);
}

const std::vector<std::string> *DeviceRegistry::linked_devices_for_class(DeviceType type) const {
  auto it = linked_remote_classes_.find(type);
  return (it != linked_remote_classes_.end()) ? &it->second : nullptr;
}

bool DeviceRegistry::apply_optimistic_target(const std::string &device_id, float target_io_position) {
  auto it = devices_.find(device_id);
  if (it == devices_.end() || !it->second.optimistic_state)
    return false;
  // Logs the raw (non-inverted) IO-protocol values the entity layer will compare against —
  // cross-check against platform_cover.cpp::on_device_update_()'s invert-aware direction math
  // when verifying a specific device's optimistic behavior.
  if (it->second.position == UNKNOWN_POSITION) {
    ESP_LOGD(TAG, "Device %s: optimistic target=%.0f (inverted=%s, last reported position=unknown)", device_id.c_str(),
             target_io_position, YESNO(it->second.inverted));
  } else {
    ESP_LOGD(TAG, "Device %s: optimistic target=%.0f (inverted=%s, last reported position=%.0f)", device_id.c_str(),
             target_io_position, YESNO(it->second.inverted), it->second.position);
  }
  it->second.optimistic.target = target_io_position;
  it->second.optimistic.motion = OptimisticState::Motion::MOVING;
  it->second.optimistic.forget_replaced_movement();  // a new command supersedes any pending STOP's
  notify(device_id);
  return true;
}

bool DeviceRegistry::apply_optimistic_stop(const std::string &device_id, bool restorable) {
  auto it = devices_.find(device_id);
  if (it == devices_.end() || !it->second.optimistic_state)
    return false;
  OptimisticState &optimistic = it->second.optimistic;
  ESP_LOGD(TAG, "Device %s: optimistic stop", device_id.c_str());
  if (!restorable) {
    optimistic.forget_replaced_movement();
  } else if (optimistic.motion == OptimisticState::Motion::MOVING) {
    // A repeated STOP (motion already STOPPED) keeps the movement the first one replaced.
    optimistic.stop_replaced_movement = true;
    optimistic.stop_replaced_target = optimistic.target;
  }
  optimistic.target = UNKNOWN_POSITION;
  optimistic.motion = OptimisticState::Motion::STOPPED;
  notify(device_id);
  return true;
}

bool DeviceRegistry::apply_optimistic_tilt(const std::string &device_id, float tilt_percent) {
  auto it = devices_.find(device_id);
  if (it == devices_.end() || !it->second.optimistic_state)
    return false;
  // Feature-gated here rather than at the call sites so every present and future caller is safe:
  // showing an angle the device will never report back would be a lie the next poll cannot
  // correct. This is the same bit known_device_accepts_execute_tilt() reads, so the two cannot
  // disagree about a given device.
  if (!has_feature(it->second.features, device_feature::TILT))
    return false;
  if (it->second.tilt == UNKNOWN_POSITION) {
    ESP_LOGD(TAG, "Device %s: optimistic tilt=%.0f (last reported tilt=unknown)", device_id.c_str(), tilt_percent);
  } else {
    ESP_LOGD(TAG, "Device %s: optimistic tilt=%.0f (last reported tilt=%.0f)", device_id.c_str(), tilt_percent,
             it->second.tilt);
  }
  it->second.optimistic.tilt = tilt_percent;
  notify(device_id);
  return true;
}

bool DeviceRegistry::rollback_optimistic(const std::string &device_id, bool failed_stop) {
  auto it = devices_.find(device_id);
  if (it == devices_.end() || it->second.optimistic.empty())
    return false;
  OptimisticState &optimistic = it->second.optimistic;
  if (failed_stop && optimistic.stop_replaced_movement) {
    ESP_LOGD(TAG, "Device %s: optimistic rollback (stop failed, movement toward %.0f resumes)", device_id.c_str(),
             optimistic.stop_replaced_target);
    const float resumed_target = optimistic.stop_replaced_target;
    optimistic.clear();
    optimistic.target = resumed_target;
    optimistic.motion = OptimisticState::Motion::MOVING;
  } else {
    ESP_LOGD(TAG, "Device %s: optimistic rollback (command failed)", device_id.c_str());
    optimistic.clear();
  }
  notify(device_id);
  return true;
}

void DeviceRegistry::confirm_optimistic_stop(const std::string &device_id) {
  auto it = devices_.find(device_id);
  if (it != devices_.end())
    it->second.optimistic.forget_replaced_movement();
}

void DeviceRegistry::for_each_linked_remote(
    const std::function<void(const std::string &, const std::vector<std::string> &)> &fn) const {
  for (const auto &pair : linked_remotes_)
    fn(pair.first, pair.second);
}

}  // namespace home_io_control
}  // namespace esphome
