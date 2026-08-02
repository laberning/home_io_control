/// @file device_registry.cpp
/// @brief DeviceRegistry implementation.
/// @ingroup hioc_hub

#include "device_registry.h"

#include "proto_frame.h"
#include "proto_sizes.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static constexpr const char *TAG = "home_io_control";

void DeviceRegistry::add(const std::string &device_id) { add(device_id, DeviceType::UNKNOWN, 0, false); }

void DeviceRegistry::add(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted,
                         bool optimistic_state) {
  if (devices_.count(device_id) != 0)
    return;
  IoDevice dev{};
  if (!hex_to_bytes(device_id, dev.node_id, NODE_ID_SIZE)) {
    ESP_LOGW(TAG, "Ignoring invalid device ID %s", device_id.c_str());
    return;
  }
  dev.type = type;
  dev.subtype = subtype;
  if (inverted)
    dev.inverted = true;
  dev.optimistic_state = optimistic_state;
  devices_[device_id] = dev;
}

void DeviceRegistry::put(const std::string &device_id, IoDevice device) { devices_[device_id] = device; }

IoDevice *DeviceRegistry::get(const std::string &device_id) {
  auto it = devices_.find(device_id);
  return (it != devices_.end()) ? &it->second : nullptr;
}

void DeviceRegistry::set_dimmable(const std::string &device_id, bool dimmable) {
  if (IoDevice *dev = this->get(device_id))
    dev->dimmable = dimmable;
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
  it->second.target = target_io_position;
  it->second.is_stopped = false;
  notify(device_id);
  return true;
}

bool DeviceRegistry::clear_optimistic_target(const std::string &device_id) {
  auto it = devices_.find(device_id);
  if (it == devices_.end() || !it->second.optimistic_state)
    return false;
  ESP_LOGD(TAG, "Device %s: optimistic stop", device_id.c_str());
  it->second.target = UNKNOWN_POSITION;
  it->second.is_stopped = true;
  notify(device_id);
  return true;
}

void DeviceRegistry::for_each_linked_remote(
    const std::function<void(const std::string &, const std::vector<std::string> &)> &fn) const {
  for (const auto &pair : linked_remotes_)
    fn(pair.first, pair.second);
}

}  // namespace home_io_control
}  // namespace esphome
