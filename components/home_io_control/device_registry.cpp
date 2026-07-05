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

void DeviceRegistry::add(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted) {
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
  devices_[device_id] = dev;
}

void DeviceRegistry::put(const std::string &device_id, IoDevice device) { devices_[device_id] = device; }

IoDevice *DeviceRegistry::get(const std::string &device_id) {
  auto it = devices_.find(device_id);
  return (it != devices_.end()) ? &it->second : nullptr;
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

void DeviceRegistry::for_each_linked_remote(
    const std::function<void(const std::string &, const std::vector<std::string> &)> &fn) const {
  for (const auto &pair : linked_remotes_)
    fn(pair.first, pair.second);
}

}  // namespace home_io_control
}  // namespace esphome
