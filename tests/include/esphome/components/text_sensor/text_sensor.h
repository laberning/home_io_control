#pragma once

#include <string>

namespace esphome {
namespace text_sensor {

#define LOG_TEXT_SENSOR(prefix, type, obj) ((void) 0)

class TextSensor {
 public:
  virtual ~TextSensor() = default;

  void publish_state(const std::string &state) { this->state = state; }

  std::string state;
};

}  // namespace text_sensor
}  // namespace esphome