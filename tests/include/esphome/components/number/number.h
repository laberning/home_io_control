#pragma once

#include "esphome/core/component.h"

namespace esphome {
namespace number {

class NumberTraits {
 public:
  void set_min_value(float) {}
  void set_max_value(float) {}
  void set_step(float) {}
  void set_unit_of_measurement(const std::string &) {}
  void set_mode(int) {}
};

class Number {
 public:
  virtual void setup() {}
  virtual void control(float value) = 0;
  void publish_state(float value) {}
  NumberTraits traits;
};

}  // namespace number
}  // namespace esphome
