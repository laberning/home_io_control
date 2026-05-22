#pragma once

namespace esphome {
namespace button {

#define LOG_BUTTON(prefix, type, obj) ((void) 0)

class Button {
 public:
  virtual ~Button() = default;
  virtual void press_action() {}

  void press() { this->press_action(); }
};

}  // namespace button
}  // namespace esphome
