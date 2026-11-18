#pragma once

#include <cmath>

namespace esphome {
namespace sensor {

#define LOG_SENSOR(prefix, type, obj) ((void) 0)

class Sensor {
 public:
  virtual ~Sensor() = default;

  void publish_state(float state) {
    this->state = state;
    this->publish_count++;
  }

  float state{NAN};
  int publish_count{0};  ///< Test stub only: how often publish_state() ran.
};

}  // namespace sensor
}  // namespace esphome
