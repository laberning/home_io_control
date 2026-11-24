#pragma once

#include <cstdint>

namespace esphome {
namespace wifi {

/// The Wi-Fi stack as far as the hub reads it: whether it is connected and on which channel.
class WiFiComponent {
 public:
  bool is_connected() const { return this->connected_; }
  int32_t get_wifi_channel() { return this->channel_; }

  // Test knobs: the stub's state, not part of the real API.
  bool connected_{false};
  int32_t channel_{0};
};

/// The hub reads the singleton; a test points it at its own instance.
inline WiFiComponent *global_wifi_component = nullptr;

}  // namespace wifi
}  // namespace esphome
