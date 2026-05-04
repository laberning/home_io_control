/// @file radio_interface.cpp
/// @brief Implementation of non-inline RadioDriver methods.

#include "radio_interface.h"
#include "esphome/core/application.h"

namespace esphome {
namespace home_io_control {

void RadioDriver::reset_hardware_() {
  if (this->rst_pin_ != nullptr) {
    this->rst_pin_->digital_write(false);
    delay(10);
    this->rst_pin_->digital_write(true);
    delay(10);
  }
}

}  // namespace home_io_control
}  // namespace esphome
