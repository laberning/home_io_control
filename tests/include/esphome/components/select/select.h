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
  void publish_state(const std::string &value) { this->state_ = value; }
  /// Test-only accessor: the last published option (the real Select reads it through current_option()).
  const std::string &get_state() const { return this->state_; }
  SelectTraits traits;

 protected:
  std::string state_;
};

}  // namespace select
}  // namespace esphome
