#pragma once

/// @file soft_phy_test_driver.h
/// @brief One shared test double for both software-PHY radio drivers (SX1262, LR1121).
///
/// TestableRadioSX1262 and TestableRadioLR1121 were byte-for-byte the same class over a different
/// base: the same IRQ-sequence scripting, the same canned-packet plumbing, the same two protected
/// overrides. This template is that class, once. Chip-specific test hooks (LR1121's
/// call_real_read_irq_status_raw()) stay in the per-chip test file as a small further subclass.

#include "radio_soft_phy_driver_base.h"
#include "radio_interface.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace test {

/// @brief Scriptable stand-in for a SoftPhyDriverBase-derived concrete driver.
/// @tparam Base esphome::home_io_control::RadioSX1262 or ::RadioLR1121.
template<class Base> class TestableSoftPhy : public Base {
 public:
  using Base::Base;

  /// Configure the sequence of IRQ status values returned by read_irq_status_raw(). uint32_t to
  /// match SoftPhyDriverBase's shared IRQ word width (each chip's own values occupy the low bits).
  void set_irq_sequence(std::initializer_list<uint32_t> seq) {
    irq_seq_.assign(seq);
    irq_idx_ = 0;
  }

  /// Set the packet read_rx_packet() should hand back when not dispatching to the real path.
  void set_expected_packet(const esphome::home_io_control::RadioRxPacket &pkt) { expected_packet_ = pkt; }

  /// Control whether the canned packet read succeeds (ignored when using the real path).
  void set_read_success(bool success) { read_success_ = success; }

  /// When true, read_rx_packet() dispatches to the real SoftPhyDriverBase implementation
  /// (exercises the actual buffer-status/read/UART-probe path against a ScriptedSpi) through the
  /// concrete driver's own primitives. LR1121 uses it; chip-neutral, so harmless for SX1262.
  void set_use_real_read_rx_packet(bool use_real) { use_real_ = use_real; }

  /// Arm the hop holdoff directly — note_reception_in_progress_() is protected on RadioDriver, and
  /// this fixture's read_rx_packet() override never reaches the branch that would arm it.
  void note_reception_from_test() { this->note_reception_in_progress_(); }

 protected:
  uint32_t read_irq_status_raw() override {
    if (irq_idx_ < irq_seq_.size())
      return irq_seq_[irq_idx_++];
    return 0;
  }

  bool read_rx_packet(esphome::home_io_control::RadioRxPacket &packet, bool blocking_wait,
                      uint32_t irq_status) override {
    if (use_real_)
      return esphome::home_io_control::SoftPhyDriverBase::read_rx_packet(packet, blocking_wait, irq_status);
    (void) blocking_wait;
    (void) irq_status;
    if (read_success_) {
      packet = expected_packet_;
      return true;
    }
    return false;
  }

 private:
  std::vector<uint32_t> irq_seq_;
  size_t irq_idx_ = 0;
  esphome::home_io_control::RadioRxPacket expected_packet_{};
  bool read_success_ = true;
  bool use_real_ = false;
};

/// @brief Scriptable stand-in for the length-driven ("early completion") receive path.
/// @tparam Base esphome::home_io_control::RadioSX1262 or ::RadioLR1121.
///
/// EarlyRxRadioSX1262 and EarlyRxRadioLR1121 were the same fixture over a different base: both
/// serve a scripted RX buffer, record every read_rx_buffer() offset/length so a test can assert
/// what the early path asked for, and widen idle_rx_completion_budget_ms() past the value the
/// host clock stubs would otherwise blow through. This is that fixture, once. The RX_DONE
/// fallback plumbing (set_fallback_packet()/fallback_used()) is chip-neutral — LR1121 tests that
/// never reach that branch simply leave it unused.
template<class Base> class EarlyRxSoftPhy : public Base {
 public:
  using Base::Base;
  using Base::early_rx_read_offset;

  void set_irq_sequence(std::initializer_list<uint32_t> seq) {
    irq_seq_.assign(seq);
    irq_idx_ = 0;
  }
  void set_rx_buffer(std::vector<uint8_t> buf) { rx_buffer_ = std::move(buf); }
  void set_fallback_packet(const esphome::home_io_control::RadioRxPacket &pkt) { fallback_packet_ = pkt; }
  void set_idle_rx_completion_budget_ms(uint32_t ms) { idle_rx_completion_budget_ms_ = ms; }

  const std::vector<uint8_t> &read_lengths() const { return read_lengths_; }
  const std::vector<uint8_t> &read_offsets() const { return read_offsets_; }
  bool fallback_used() const { return fallback_used_; }

 protected:
  uint32_t read_irq_status_raw() override {
    if (irq_idx_ < irq_seq_.size())
      return irq_seq_[irq_idx_++];
    return 0;
  }

  void read_rx_buffer(uint8_t offset, uint8_t *data, uint8_t len) override {
    read_offsets_.push_back(offset);
    read_lengths_.push_back(len);
    for (uint8_t i = 0; i < len; i++)
      data[i] = i < rx_buffer_.size() ? rx_buffer_[i] : 0x00;
  }

  bool read_rx_packet(esphome::home_io_control::RadioRxPacket &packet, bool blocking_wait,
                      uint32_t irq_status) override {
    (void) blocking_wait;
    (void) irq_status;
    fallback_used_ = true;
    packet = fallback_packet_;
    return packet.len > 0;
  }

  uint32_t idle_rx_completion_budget_ms() const override { return idle_rx_completion_budget_ms_; }

 private:
  std::vector<uint32_t> irq_seq_;
  size_t irq_idx_ = 0;
  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> read_lengths_;
  std::vector<uint8_t> read_offsets_;
  esphome::home_io_control::RadioRxPacket fallback_packet_{};
  bool fallback_used_ = false;
  uint32_t idle_rx_completion_budget_ms_ = 20000;
};

}  // namespace test
