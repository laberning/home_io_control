#pragma once

#include "radio_interface.h"
#include "radio_sx1262.h"  // SX1262_RESPONSE_PREAMBLE for the SX1262 mock
#include <esphome/core/gpio.h>

#include <deque>
#include <optional>
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

  // Configured pin flags, as if set once by codegen from YAML (e.g. pullup:/pulldown:) --
  // returned by get_flags() regardless of any pin_mode() calls that happen afterward. This
  // mirrors real ESP32InternalGPIOPin::pin_mode(), which only programs the IDF GPIO driver and
  // never mutates the pin's own flags_ member; get_flags() always reads back the configuration,
  // not the pin's current transient direction. See BusyPinAsResetStrapGuard
  // (radio_lr1121_firmware_updater.cpp) for the fix this distinction is needed to test.
  void set_flags(esphome::gpio::Flags flags) { configured_flags_ = flags; }
  esphome::gpio::Flags get_flags() const override { return configured_flags_; }
  // Records every pin_mode() call (direction flip to OUTPUT, then whatever is restored on the
  // way back) so tests can assert what a RAII guard restored, not just that it restored *something*.
  void pin_mode(esphome::gpio::Flags flags) override { pin_mode_calls_.push_back(flags); }
  const std::vector<esphome::gpio::Flags> &pin_mode_calls() const { return pin_mode_calls_; }

 private:
  bool value_;
  esphome::gpio::Flags configured_flags_{esphome::gpio::FLAG_INPUT};
  std::vector<esphome::gpio::Flags> pin_mode_calls_;
};

// --- Mock radio driver (generic, queue-based) ---------------------------------
class MockRadio : public esphome::home_io_control::RadioDriver {
 public:
  MockRadio() : send_count_(0) {}

  /// One entry per wait_for_packet()/change_frequency() call, in call order. freq_history() and
  /// wait_timeouts() each record only their own calls, so neither can answer "did it hop before
  /// or after its first listen" for a loop that keeps running after a successful reply (e.g.
  /// collect_broadcast_responses(), which collects for its whole window rather than returning on
  /// the first match) — this interleaved log can.
  enum class CallKind { kWait, kHop };

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
    // Real drivers retune the receiver to the TX frequency as a side effect of sending (e.g.
    // RadioSX1276::send_packet() calls change_frequency(); SoftPhyDriverBase::send_packet() calls
    // set_frequency_register(), which assigns current_freq_ the same way). Assigned directly here,
    // not via change_frequency(), so it does not appear in freq_history() — that vector must keep
    // meaning "retunes the engine explicitly asked for while listening", not "every frequency the
    // radio has ever been on".
    current_freq_ = tx_config.freq_hz;
    return result;
  }
  bool wait_for_packet(esphome::home_io_control::RadioRxPacket &packet, uint32_t timeout_ms) override {
    wait_timeouts_.push_back(timeout_ms);
    call_log_.push_back(CallKind::kWait);
    if (emulate_capture_lifecycle_)
      this->clear_last_capture_();
    if (rx_queue_.empty()) {
      return false;
    }
    std::optional<esphome::home_io_control::RadioRxPacket> entry = rx_queue_.front();
    rx_queue_.pop_front();
    if (!entry.has_value()) {
      return false;  // Queued silence: a slice that times out with nothing received.
    }
    packet = *entry;
    if (emulate_capture_lifecycle_)
      this->populate_capture_base_(true, packet.freq_hz, -55, packet.data, packet.len, packet.data, packet.len);
    return true;
  }
  bool check_for_packet(esphome::home_io_control::RadioRxPacket &packet) override {
    (void) packet;
    return false;
  }
  void change_frequency(uint32_t freq_hz) override {
    freq_history_.push_back(freq_hz);
    call_log_.push_back(CallKind::kHop);
    current_freq_ = freq_hz;
  }
  int16_t read_rssi() override {
    if (rssi_queue_.empty())
      return rssi_default_;
    int16_t val = rssi_queue_.front();
    rssi_queue_.pop_front();
    return val;
  }
  bool is_sync_detected() override { return false; }
  bool is_preamble_detected() override { return false; }
  // hop_dwell_ms and has_fast_tx_rx_turnaround are pure virtual in RadioDriver; the
  // generic mock behaves like the fast-hopping, fast-turnaround reference platform so existing
  // discovery-timing and key-exchange flow tests keep exercising the standard paths.
  uint16_t hop_dwell_ms(const esphome::home_io_control::TuningConfig &tuning) const override {
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
  /// Queue `n` empty slices: wait_for_packet() returns false for each, exactly as if nothing had
  /// arrived, without needing to leave the whole queue empty (which a test can't do selectively
  /// mid-sequence). Lets a test express "several genuinely silent waits, then a reply" — distinct
  /// from queuing a frame that gets rejected for an unrelated reason (endpoint mismatch, wrong
  /// command), which consumes a wait_for_packet() call immediately instead of leaving it empty and
  /// so has a different timing profile.
  void queue_rx_silence(uint8_t n = 1) {
    for (uint8_t i = 0; i < n; i++)
      rx_queue_.push_back(std::nullopt);
  }
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
  /// Emulate the real drivers' capture lifecycle: every wait_for_packet() clears the capture
  /// before listening, and a delivered frame populates it. Off by default because most tests stage
  /// a capture directly via set_last_capture_rssi() and would simply have it wiped; on for tests
  /// that care a later empty wait must not erase an earlier informative one.
  void set_emulate_capture_lifecycle(bool on) { emulate_capture_lifecycle_ = on; }
  int get_send_count() const { return send_count_; }
  /// Every timeout the engine handed to wait_for_packet(), in order. Lets tests observe the
  /// response window an exchange budgeted without depending on the host clock stubs.
  const std::vector<uint32_t> &wait_timeouts() const { return wait_timeouts_; }
  /// Every channel the engine retuned to, in order. Channel policy — which loops hop, which stay
  /// put, and which channels a hopping loop visits — is otherwise invisible to tests: only the
  /// final frequency survives, and every wait loop ends on some frequency. An empty history means
  /// the engine never called change_frequency(), not that it is on no channel: RadioDriver's
  /// current_freq_ starts at FREQ_CH2 without one.
  const std::vector<uint32_t> &freq_history() const { return freq_history_; }
  /// See the `CallKind` doc comment above: interleaved wait/hop call order, for loops where
  /// presence-in-freq_history() alone can't distinguish "hopped before the first listen" from
  /// "hopped after a later one".
  const std::vector<CallKind> &call_log() const { return call_log_; }
  const std::vector<esphome::home_io_control::RadioTxConfig> &get_tx_configs() const { return tx_configs_; }
  const std::vector<std::vector<uint8_t>> &get_sent_data() const { return sent_data_; }
  void clear() {
    rx_queue_.clear();
    tx_results_.clear();
    tx_configs_.clear();
    rssi_queue_.clear();
    sent_data_.clear();
    send_count_ = 0;
    freq_history_.clear();
    wait_timeouts_.clear();
    call_log_.clear();
  }

 private:
  std::deque<bool> tx_results_;
  std::deque<std::optional<esphome::home_io_control::RadioRxPacket>> rx_queue_;
  std::deque<int16_t> rssi_queue_;
  std::vector<esphome::home_io_control::RadioTxConfig> tx_configs_;
  std::vector<std::vector<uint8_t>> sent_data_;
  std::vector<CallKind> call_log_;
  int16_t rssi_default_{-120};
  int send_count_;
  std::vector<uint32_t> wait_timeouts_;
  std::vector<uint32_t> freq_history_;
  bool emulate_capture_lifecycle_{false};
};

// --- SX1262 mock — inherits MockRadio but overrides chip-specific behavior ----
/// Simulates SX1262 response_preamble() returning the longer preamble.
class MockRadioSX1262 : public MockRadio {
 public:
  const char *chip_name() const override { return "sx1262"; }
  uint16_t response_preamble() const override { return esphome::home_io_control::SX1262_RESPONSE_PREAMBLE; }
  uint16_t hop_dwell_ms(const esphome::home_io_control::TuningConfig &tuning) const override {
    return tuning.sx1262_discovery_hop_slice_ms;
  }
  bool has_fast_tx_rx_turnaround() const override { return false; }
};
