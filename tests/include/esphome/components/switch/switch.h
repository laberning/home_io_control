#pragma once

#include <esphome/core/component.h>

namespace esphome {
namespace switch_ {

#define LOG_SWITCH(prefix, type, obj) ((void) 0)

class Switch {
 public:
  virtual ~Switch() = default;
  virtual void write_state(bool state) = 0;

  void publish_state(bool state) { this->state_ = state; }
  bool get_state() const { return this->state_; }

 protected:
  bool state_{false};
};

}  // namespace switch_
}  // namespace esphome
