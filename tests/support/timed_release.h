#pragma once

/// @file timed_release.h
/// @brief When a replayed reply arrives, measured from the transmission it answers.
///
/// A test double that replays a captured timeline records every send it makes in a
/// SendTimeline and asks it when a queued reply is due. The rule lives here, apart from any one
/// double, so every double that replays a timeline (MockRadio today) releases replies the same
/// way.

#include <esphome/core/hal.h>  // test_clock

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/// When a timed reply arrives: a fixed offset after the start of the send it answers. The anchor
/// is either the N-th send (0-based) or the first send whose bytes equal `anchor_bytes`.
struct TimedRelease {
  std::optional<int> anchor_send_index;
  std::vector<uint8_t> anchor_bytes;
  uint64_t offset_us{0};
};

/// Every send so far, in order: its bytes and the manual-clock time it started at (0 under the
/// legacy clock, where no timed release is meaningful).
class SendTimeline {
 public:
  /// Record one send. Call it when the transmission starts, before any modelled air time moves
  /// the clock, so release offsets stay measured from the start of the transmission.
  void record(const uint8_t *data, uint8_t len) {
    sent_data_.emplace_back(data, data + len);
    start_us_.push_back(esphome::test_clock::is_manual() ? esphome::test_clock::state().now_us : 0);
  }

  /// Absolute release time of @p timed, or nullopt while its anchor send hasn't happened.
  std::optional<uint64_t> release_us(const TimedRelease &timed) const {
    if (timed.anchor_send_index.has_value()) {
      const auto index = static_cast<size_t>(*timed.anchor_send_index);
      if (index >= start_us_.size())
        return std::nullopt;
      return start_us_[index] + timed.offset_us;
    }
    for (size_t i = 0; i < sent_data_.size(); i++) {
      if (sent_data_[i] == timed.anchor_bytes)
        return start_us_[i] + timed.offset_us;
    }
    return std::nullopt;
  }

  const std::vector<std::vector<uint8_t>> &sent_data() const { return sent_data_; }
  /// Start time of each send, parallel to sent_data().
  const std::vector<uint64_t> &start_us() const { return start_us_; }

  void clear() {
    sent_data_.clear();
    start_us_.clear();
  }

 private:
  std::vector<std::vector<uint8_t>> sent_data_;
  std::vector<uint64_t> start_us_;
};
