#pragma once

#include <cmath>

namespace esphome {
namespace sensor {

#define LOG_SENSOR(prefix, type, obj) ((void) 0)

class Sensor {
 public:
  virtual ~Sensor() = default;

  void publish_state(float state) { this->state = state; }

  float state{NAN};
};

}  // namespace sensor
}  // namespace esphome
