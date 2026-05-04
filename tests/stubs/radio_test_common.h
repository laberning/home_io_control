#pragma once

#include "radio_interface.h"
#include <esphome/core/gpio.h>

#include <deque>

// ============================================================================
// Common test doubles for radio drivers
// ============================================================================

// --- Mock SPI ---------------------------------------------------------------
class MockSpi : public esphome::home_io_control::SpiAccess {
 public:
  void spi_enable() override {}
  void spi_disable() override {}
  uint8_t spi_transfer(uint8_t data) override { return 0; }
  void spi_write(uint8_t data) override {}
  uint8_t spi_read() override { return 0; }
};

// --- Mock GPIO --------------------------------------------------------------
class MockPin : public esphome::InternalGPIOPin {
 public:
  explicit MockPin(bool initial = false) : value_(initial) {}
  bool digital_read() override { return value_; }
  void set_value(bool v) { value_ = v; }

 private:
  bool value_;
};

// --- Mock radio driver (generic, queue-based) ---------------------------------
class MockRadio : public esphome::home_io_control::RadioDriver {
 public:
  MockRadio() : send_count_(0) {}

  // RadioDriver interface
  bool init() override { return true; }
  bool send_packet(const uint8_t *data, uint8_t len,
                   const esphome::home_io_control::RadioTxConfig &tx_config) override {
    (void) data;
    (void) len;
    (void) tx_config;
    bool result = true;
    if (!tx_results_.empty()) {
      result = tx_results_.front();
      tx_results_.pop_front();
    }
    send_count_++;
    return result;
  }
  bool wait_for_packet(esphome::home_io_control::RadioRxPacket &packet, uint32_t timeout_ms) override {
    (void) timeout_ms;
    if (rx_queue_.empty()) {
      return false;
    }
    packet = rx_queue_.front();
    rx_queue_.pop_front();
    return true;
  }
  bool check_for_packet(esphome::home_io_control::RadioRxPacket &packet) override {
    (void) packet;
    return false;
  }
  void change_frequency(uint32_t freq_hz) override { current_freq_ = freq_hz; }
  int16_t read_rssi() override {
    if (rssi_queue_.empty())
      return rssi_default_;
    int16_t val = rssi_queue_.front();
    rssi_queue_.pop_front();
    return val;
  }
  void set_mode_rx() override {}
  void set_mode_standby() override {}
  bool is_failed() const override { return false; }
  const char *chip_name() const override { return "MockRadio"; }
  void dump_debug() override {}

  // Test helpers
  void queue_rx(const esphome::home_io_control::RadioRxPacket &pkt) { rx_queue_.push_back(pkt); }
  void queue_tx_result(bool success) { tx_results_.push_back(success); }
  void queue_rssi(int16_t rssi) { rssi_queue_.push_back(rssi); }
  void set_rssi_default(int16_t rssi) { rssi_default_ = rssi; }
  int get_send_count() const { return send_count_; }
  void clear() {
    rx_queue_.clear();
    tx_results_.clear();
    rssi_queue_.clear();
    send_count_ = 0;
  }

 private:
  std::deque<bool> tx_results_;
  std::deque<esphome::home_io_control::RadioRxPacket> rx_queue_;
  std::deque<int16_t> rssi_queue_;
  int16_t rssi_default_{-120};
  int send_count_;
};
