#pragma once

/// @file fake_byte_stream.h
/// @brief A ByteStreamAccess double for driver tests: scripted bytes in, recorded bytes out, and
///        a clock that runs in lockstep with the polling.
///
/// Time under test is manual, so a driver's wait loop only ends if something advances the clock.
/// This double does it the way a real link does: an empty poll takes time. Every available() or
/// read() that finds nothing to deliver advances the manual clock by one tick and gives the peer
/// (the pump hook) a chance to emit whatever has become due; the poll then returns what arrived
/// in that tick. A genuine hang still ends at the test's time limit instead of spinning.

#include <esphome/core/hal.h>  // test_clock

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "radio_interface.h"

class FakeByteStream : public esphome::home_io_control::ByteStreamAccess {
 public:
  /// Clock advance of one empty poll: about half a byte time at 1 Mbaud, fine enough for the
  /// 1 ms-order timing of the protocol and coarse enough that a 5 s handshake is 100 000 polls.
  static constexpr uint64_t TICK_US = 50;

  // === The peer's side ===

  /// Bytes the peer sends to the host; they become readable at once.
  void push_rx(const uint8_t *data, size_t n) { rx_.insert(rx_.end(), data, data + n); }
  void push_rx(const std::vector<uint8_t> &data) { push_rx(data.data(), data.size()); }
  /// Deliver at most `n` bytes per available()/read() (0 = no limit), so a test sees frames
  /// arrive in pieces.
  void set_bytes_per_read(size_t n) { bytes_per_read_ = n; }
  /// Called before every poll and again after a tick; the peer moves due events into the queue.
  void set_pump(std::function<void()> pump) { pump_ = std::move(pump); }
  /// Called with every write(), so a peer can answer a command as it arrives.
  void set_write_handler(std::function<void(const uint8_t *, size_t)> handler) { on_write_ = std::move(handler); }
  /// The line rate the driver sees (default: the spec's 1 Mbaud).
  void set_baud_rate(uint32_t baud) { baud_rate_ = baud; }
  [[nodiscard]] uint32_t baud_rate() const override { return baud_rate_; }
  /// Bytes still waiting to be read.
  [[nodiscard]] size_t queued() const { return rx_.size(); }
  /// Polls that found nothing and therefore advanced the clock.
  [[nodiscard]] size_t empty_polls() const { return empty_polls_; }

  // === What the host did ===

  [[nodiscard]] const std::vector<uint8_t> &written() const { return written_; }
  [[nodiscard]] size_t flush_count() const { return flush_count_; }
  void clear_written() { written_.clear(); }

  // === ByteStreamAccess ===

  size_t available() override {
    poll_();
    return deliverable_();
  }

  size_t read(uint8_t *buf, size_t max) override {
    poll_();
    size_t const n = std::min(deliverable_(), max);
    for (size_t i = 0; i < n; i++) {
      buf[i] = rx_.front();
      rx_.pop_front();
    }
    return n;
  }

  void write(const uint8_t *buf, size_t n) override {
    written_.insert(written_.end(), buf, buf + n);
    if (on_write_)
      on_write_(buf, n);
  }

  void flush() override { flush_count_++; }

 private:
  /// Runs the pump; when nothing is deliverable, spends one tick and runs it again.
  void poll_() {
    if (pump_)
      pump_();
    if (!rx_.empty())
      return;
    empty_polls_++;
    if (esphome::test_clock::is_manual())
      esphome::test_clock::advance_us(TICK_US);
    if (pump_)
      pump_();
  }

  [[nodiscard]] size_t deliverable_() const {
    return bytes_per_read_ == 0 ? rx_.size() : std::min(rx_.size(), bytes_per_read_);
  }

  std::deque<uint8_t> rx_;
  std::vector<uint8_t> written_;
  std::function<void()> pump_;
  std::function<void(const uint8_t *, size_t)> on_write_;
  size_t bytes_per_read_{0};
  uint32_t baud_rate_{1000000};
  size_t flush_count_{0};
  size_t empty_polls_{0};
};
