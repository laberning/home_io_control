#pragma once

namespace esphome {
namespace button {

class Button {
 public:
  virtual ~Button() = default;
  virtual void press_action() {}
};

}  // namespace button
}  // namespace esphome
