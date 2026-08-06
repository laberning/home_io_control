#pragma once

#include "radio_interface.h"

#include <initializer_list>
#include <vector>

// ============================================================================
// Scripted SPI: records each transaction (spi_enable..spi_disable) as its own byte
// vector and plays back a queued sequence of response bytes across all transactions.
//
// MockSpi (radio_test_common.h) always returns 0 and does not record writes, so it cannot
// support the byte-exact assertions two-transaction command protocols need (LR1121's normal
// driver, and the bootloader-mode updater). Shared by tests/radio_lr1121_test.cpp and
// tests/radio_lr1121_firmware_updater_test.cpp — both need the identical byte-exact,
// scriptable-response shape, just against different command sets.
// ============================================================================

class ScriptedSpi : public esphome::home_io_control::SpiAccess {
 public:
  void spi_enable() override { current_.clear(); }
  void spi_disable() override { transactions_.push_back(current_); }
  uint8_t spi_transfer(uint8_t data) override {
    current_.push_back(data);
    return this->next_response_();
  }
  void spi_write(uint8_t data) override { current_.push_back(data); }
  uint8_t spi_read() override {
    current_.push_back(0);
    return this->next_response_();
  }

  // Queue response bytes played back in order across every subsequent transfer/read.
  void queue_response(uint8_t byte) { response_queue_.push_back(byte); }
  void queue_responses(std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes)
      response_queue_.push_back(b);
  }

  // Every completed transaction (one spi_enable..spi_disable cycle) as its written bytes.
  const std::vector<std::vector<uint8_t>> &transactions() const { return transactions_; }

  // Find the first transaction whose bytes start with the given 16-bit opcode.
  // Returns -1 if not found.
  int find_opcode(uint16_t opcode) const {
    uint8_t msb = (opcode >> 8) & 0xFF;
    uint8_t lsb = opcode & 0xFF;
    for (size_t i = 0; i < transactions_.size(); i++) {
      if (transactions_[i].size() >= 2 && transactions_[i][0] == msb && transactions_[i][1] == lsb)
        return static_cast<int>(i);
    }
    return -1;
  }

 private:
  uint8_t next_response_() {
    if (read_idx_ < response_queue_.size())
      return response_queue_[read_idx_++];
    return 0;
  }

  std::vector<uint8_t> current_;
  std::vector<std::vector<uint8_t>> transactions_;
  std::vector<uint8_t> response_queue_;
  size_t read_idx_{0};
};
