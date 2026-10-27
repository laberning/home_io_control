#pragma once

/// @file testable_radio_bridle.h
/// @brief RadioBridle test double: recorders for what the hub asked of the driver, counters
///        opened up, and a reset line that boots a FakeCoprocessor.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "esphome/core/gpio.h"
#include "radio_bridle.h"

/// The co-processor's active-low reset input: releasing it (low → high) runs `on_release`, which a
/// test points at FakeCoprocessor::boot(). Records the time of every pulse.
class FakeResetLine : public esphome::InternalGPIOPin {
 public:
  void digital_write(bool value) override {
    if (!value && level_)
      pulse_starts_us.push_back(esphome::test_clock::state().now_us);
    if (value && !level_ && on_release)
      on_release();
    level_ = value;
  }
  bool digital_read() override { return level_; }

  std::function<void()> on_release;
  std::vector<uint64_t> pulse_starts_us;  ///< Manual-clock time each low pulse began.

 private:
  bool level_{true};
};

class TestableRadioBridle : public esphome::home_io_control::RadioBridle {
 public:
  using RadioBridle::RadioBridle;
  using RadioBridle::Link;
  using RadioBridle::set_trace;

  ~TestableRadioBridle() override { write_trace_file_(); }

  // === Recorders: what the hub handed to the driver ===

  esphome::home_io_control::TxResult send_packet(const uint8_t *data, uint8_t len,
                                                 const esphome::home_io_control::RadioTxConfig &cfg) override {
    send_times_ms.push_back(esphome::test_clock::peek_ms());
    esphome::home_io_control::TxResult const result = RadioBridle::send_packet(data, len, cfg);
    if (result.sent()) {  // as MockRadio: a send that did not go out records nothing
      sent_frames.emplace_back(data, data + len);
      tx_configs.push_back(cfg);
    }
    return result;
  }
  bool wait_for_packet(esphome::home_io_control::RadioRxPacket &packet, uint32_t timeout_ms) override {
    wait_timeouts.push_back(timeout_ms);
    return RadioBridle::wait_for_packet(packet, timeout_ms);
  }
  void change_frequency(uint32_t freq_hz) override {
    freq_history.push_back(freq_hz);
    RadioBridle::change_frequency(freq_hz);
  }

  std::vector<std::vector<uint8_t>> sent_frames;  ///< io frames of the sends that went out.
  std::vector<uint32_t> send_times_ms;            ///< peek_ms() at every send_packet() call, sent or not.
  std::vector<uint32_t> wait_timeouts;            ///< Every timeout handed to wait_for_packet().
  std::vector<esphome::home_io_control::RadioTxConfig> tx_configs;  ///< Configs of the sends that went out.
  std::vector<uint32_t> freq_history;

  // === Trace ===

  /// When the environment names a trace directory (HIOC_BRIDLE_TRACE_DIR), trace this driver and
  /// write its lines to `<dir>/<suite>.<test>.log` when it goes away, the raw material of session
  /// fixtures and of the trace lint. Unset, nothing is traced and nothing is written, so a plain
  /// test run has no side effects. Opt-in per rig: a test that provokes faults on purpose (a silent
  /// co-processor) is not a trace the lint should judge.
  void trace_to_environment_dir() {
    const char *dir = std::getenv("HIOC_BRIDLE_TRACE_DIR");
    if (dir == nullptr || *dir == '\0')
      return;
    trace_dir_ = dir;
    set_trace(true);
  }

  std::vector<std::string> trace_lines;  ///< Every `[bridle]` line the driver emitted.
  /// Called after every emitted trace line: a test makes it advance the manual clock to model a log
  /// UART that blocks for the time a line takes to send.
  std::function<void()> on_trace_line;

  // === Opened up for assertions ===

  [[nodiscard]] bool running() const { return link_running(); }
  [[nodiscard]] size_t fifo_count() const { return fifo_count_; }
  [[nodiscard]] bool scanning() const { return scanning_; }
  /// Whether the driver believes the radio receives (a plain `rx_on`, a scan or a transmission's `then_rx`).
  [[nodiscard]] bool rx_on_active() const { return rx_on_; }
  /// Unawaited commands (`rx_on`, `rx_scan`, `idle`) still waiting for their `ack` or `error`.
  [[nodiscard]] size_t pending_count() const { return pending_count_; }
  /// Errors received for unawaited commands.
  [[nodiscard]] uint32_t unawaited_errors() const { return unawaited_errors_; }
  /// The code and detail of the last `error` that ended a wait (for `set_params`, detail is the TLV id).
  [[nodiscard]] esphome::home_io_control::bridle::ErrorCode last_error_code() const { return last_error_code_; }
  [[nodiscard]] int16_t last_error_detail() const { return last_error_detail_; }
  [[nodiscard]] uint8_t selected_radio() const { return radio_; }
  [[nodiscard]] int8_t applied_tx_power_dbm() const { return applied_tx_power_dbm_; }
  [[nodiscard]] uint32_t dropped_before_tx_done() const { return dropped_before_tx_done_; }
  [[nodiscard]] uint32_t fifo_overflows() const { return fifo_overflows_; }
  [[nodiscard]] uint32_t oversize_dropped() const { return oversize_dropped_; }
  [[nodiscard]] uint32_t header_mismatches() const { return header_mismatches_; }
  [[nodiscard]] uint32_t bad_crc_frames() const { return bad_crc_frames_; }
  [[nodiscard]] uint32_t short_frames() const { return short_frames_; }
  [[nodiscard]] uint32_t strobe_late_total() const { return strobe_late_total_; }
  [[nodiscard]] uint32_t counter_gaps() const { return counter_gaps_; }
  [[nodiscard]] uint32_t drains_cut() const { return drains_cut_; }
  [[nodiscard]] uint32_t resyncs() const { return resyncs_; }
  [[nodiscard]] bool have_stats() const { return have_stats_; }
  [[nodiscard]] uint32_t log_dropped() const { return log_dropped_; }
  [[nodiscard]] bool have_rx_short() const { return have_rx_short_; }
  [[nodiscard]] uint32_t rx_short() const { return rx_short_; }
  [[nodiscard]] uint32_t stale_replies() const { return stale_replies_; }

 protected:
  void emit_trace_line(const char *line) override {
    trace_lines.emplace_back(line);
    if (on_trace_line)
      on_trace_line();
  }

 private:
  void write_trace_file_() {
    if (trace_dir_.empty() || trace_lines.empty())
      return;
    const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info != nullptr ? std::string(info->test_suite_name()) + "." + info->name() : "unknown";
    std::replace(name.begin(), name.end(), '/', '_');
    std::filesystem::create_directories(trace_dir_);
    std::ofstream out(std::filesystem::path(trace_dir_) / (name + ".log"));
    for (const std::string &line : trace_lines)
      out << line << '\n';
  }

  std::string trace_dir_;
};
