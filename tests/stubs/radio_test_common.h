#pragma once

#include "radio_interface.h"
#include "radio_sx1262.h"  // SX1262_EXCHANGE_RESPONSE_WAIT_SLICE_MS for the SX1262 mock
#include <esphome/core/gpio.h>

#include <deque>
#include <vector>

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
    bool result = true;
    if (!tx_results_.empty()) {
      result = tx_results_.front();
      tx_results_.pop_front();
    }
    tx_configs_.push_back(tx_config);
    sent_data_.push_back(std::vector<uint8_t>(data, data + len));
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
  bool is_sync_detected() override { return false; }
  bool is_preamble_detected() override { return false; }
  // discovery_hop_slice_ms and has_fast_tx_rx_turnaround are pure virtual in RadioDriver; the
  // generic mock behaves like the fast-hopping, fast-turnaround reference platform so existing
  // discovery-timing and key-exchange flow tests keep exercising the standard paths.
  uint16_t discovery_hop_slice_ms(const esphome::home_io_control::TuningConfig &tuning) const override {
    return tuning.sx1276_discovery_hop_slice_ms;
  }
  bool has_fast_tx_rx_turnaround() const override { return true; }
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
  // Stage a valid get_last_capture() for tests exercising the link-health RSSI path. Real drivers
  // populate this via populate_capture_base_() inside wait_for_packet()/check_for_packet(); this
  // generic mock overrides both fully with queue-based logic and never calls it, so tests that
  // need a capture (rather than just an RX frame) set it directly instead.
  void set_last_capture_rssi(int16_t rssi_dbm) {
    this->populate_capture_base_(false, 0, rssi_dbm, nullptr, 0, nullptr, 0);
  }
  int get_send_count() const { return send_count_; }
  const std::vector<esphome::home_io_control::RadioTxConfig> &get_tx_configs() const { return tx_configs_; }
  const std::vector<std::vector<uint8_t>> &get_sent_data() const { return sent_data_; }
  void clear() {
    rx_queue_.clear();
    tx_results_.clear();
    tx_configs_.clear();
    rssi_queue_.clear();
    sent_data_.clear();
    send_count_ = 0;
  }

 private:
  std::deque<bool> tx_results_;
  std::deque<esphome::home_io_control::RadioRxPacket> rx_queue_;
  std::deque<int16_t> rssi_queue_;
  std::vector<esphome::home_io_control::RadioTxConfig> tx_configs_;
  std::vector<std::vector<uint8_t>> sent_data_;
  int16_t rssi_default_{-120};
  int send_count_;
};

// --- SX1262 mock — inherits MockRadio but overrides chip-specific behavior ----
/// Simulates SX1262 response_preamble() returning the longer preamble.
class MockRadioSX1262 : public MockRadio {
 public:
  const char *chip_name() const override { return "sx1262"; }
  uint16_t response_preamble() const override { return esphome::home_io_control::SX1262_RESPONSE_PREAMBLE; }
  uint32_t exchange_wait_slice_ms() const override {
    return esphome::home_io_control::SX1262_EXCHANGE_RESPONSE_WAIT_SLICE_MS;
  }
  uint16_t discovery_hop_slice_ms(const esphome::home_io_control::TuningConfig &tuning) const override {
    return tuning.sx1262_discovery_hop_slice_ms;
  }
  bool has_fast_tx_rx_turnaround() const override { return false; }
};
