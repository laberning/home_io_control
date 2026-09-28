/// @file radio_interface.cpp
/// @brief Implementation of non-inline RadioDriver methods.
/// @ingroup hioc_radio

#include "radio_interface.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.radio";

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

std::optional<int16_t> RadioDriver::channel_busy_level_(const RadioTxConfig &tx_config, uint32_t settle_us) {
  if (!tx_config.cca_threshold_dbm.has_value())
    return std::nullopt;
  // The idle receiver hops, so it is usually on another channel than the one about to be used:
  // measure where the frame will go, not where the receiver happens to be.
  if (this->current_freq_ != tx_config.freq_hz) {
    uint32_t const start_us = micros();
    this->change_frequency(tx_config.freq_hz);
    delayMicroseconds(settle_us);
    if (!this->cca_retune_logged_) {
      this->cca_retune_logged_ = true;
      ESP_LOGD(TAG, "Clear-channel check retuned to %" PRIu32 " Hz: %" PRIu32 " us including %" PRIu32 " us settle",
               tx_config.freq_hz, micros() - start_us, settle_us);
    }
  }
  int16_t const level = this->read_rssi();
  if (level >= *tx_config.cca_threshold_dbm)
    return level;
  return std::nullopt;
}

}  // namespace home_io_control
}  // namespace esphome
