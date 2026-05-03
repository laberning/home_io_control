/// @file radio_sx1262_stub.cpp
/// @brief Minimal stub of RadioSX1262 for host unit tests.

#include "radio_sx1262.h"

namespace esphome {
namespace home_io_control {

bool RadioSX1262::init() { return true; }

bool RadioSX1262::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  (void) data;
  (void) len;
  (void) tx_config;
  return true;
}

bool RadioSX1262::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  (void) packet;
  (void) timeout_ms;
  return false;
}

bool RadioSX1262::check_for_packet(RadioRxPacket &packet) {
  (void) packet;
  return false;
}

void RadioSX1262::change_frequency(uint32_t freq_hz) { (void) freq_hz; }

void RadioSX1262::set_mode_rx() {}

void RadioSX1262::set_mode_standby() {}

void RadioSX1262::dump_debug() {}

}  // namespace home_io_control
}  // namespace esphome
