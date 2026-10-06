#pragma once

namespace esphome {
namespace binary_sensor {

#define LOG_BINARY_SENSOR(prefix, type, obj) ((void) 0)

class BinarySensor {
 public:
  virtual ~BinarySensor() = default;

  void publish_state(bool new_state) {
    this->state = new_state;
    this->has_state_ = true;
  }
  // Real ESPHome inherits these two from StatefulEntityBase / EntityBase.
  void invalidate_state() { this->has_state_ = false; }
  bool has_state() const { return this->has_state_; }

  bool state{};

 protected:
  bool has_state_{false};
};

}  // namespace binary_sensor
}  // namespace esphome
