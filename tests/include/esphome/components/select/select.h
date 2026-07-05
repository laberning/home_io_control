#pragma once

#include "esphome/core/component.h"
#include <string>
#include <vector>

namespace esphome {
namespace select {

class SelectTraits {
 public:
  void set_options(const std::vector<std::string> &) {}
};

class Select {
 public:
  virtual void setup() {}
  virtual void control(const std::string &value) = 0;
  void publish_state(const std::string &value) {}
  SelectTraits traits;
};

}  // namespace select
}  // namespace esphome
