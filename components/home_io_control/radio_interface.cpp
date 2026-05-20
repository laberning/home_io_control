/// @file radio_interface.cpp
/// @brief Implementation of non-inline RadioDriver methods.
/// @ingroup hioc_radio

#include "radio_interface.h"
#include "esphome/core/application.h"

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint32_t RADIO_RESET_PULSE_DELAY_MS =
    10;  ///< Reset low/high pulse used by both radio drivers during hardware reset.

}  // namespace

void RadioDriver::reset_hardware_() {
  if (this->rst_pin_ != nullptr) {
    this->rst_pin_->digital_write(false);
    delay(RADIO_RESET_PULSE_DELAY_MS);
    this->rst_pin_->digital_write(true);
    delay(RADIO_RESET_PULSE_DELAY_MS);
  }
}

}  // namespace home_io_control
}  // namespace esphome
