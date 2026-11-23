#pragma once

/// @file channel_quiet_level.h
/// @brief What the board hears on an idle channel: a per-channel tally of single RSSI readings, the
/// quiet level taken from it, and the bookkeeping of what is worth logging about the limit derived
/// from it (ADR 0096).
/// @ingroup hioc_hub
///
/// Pure and clock-free: the exchange engine feeds it the readings it takes at its idle hop and asks it
/// for the quiet level; a test feeds numbers. The rule that turns a quiet level into a busy limit is
/// decisions::busy_limit_dbm() (decisions_channel.h).

#include "decisions_channel.h"
#include "proto_timing.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace esphome {
namespace home_io_control {

/// A channel and the quiet level measured on it.
struct ChannelLevel {
  uint32_t freq_hz{0};   ///< The channel.
  int16_t level_dbm{0};  ///< Its quiet level.
};

/// @brief Histogram of single idle RSSI readings per request channel, and the quiet level it gives.
///
/// A channel's readings are binned at 1 dB. Once a channel holds QUIET_LEVEL_HALVING_READINGS readings
/// all its bins are halved, so old readings fade and the median follows a change over a few minutes.
/// The quiet level of a channel is the median of its readings, known from QUIET_LEVEL_MIN_READINGS.
/// The board's quiet level is the lowest of the three channels': channels 2 and 3 carry foreign
/// traffic that lifts their median, and the quietest channel says what the receiver itself hears.
class ChannelQuietLevel {
 public:
  /// Add one reading taken on @p freq_hz. A frequency that is none of the three channels, and a
  /// reading outside the tally's range (clamped to its end bins), are handled safely.
  void add_reading(uint32_t freq_hz, int16_t level_dbm) {
    const std::optional<size_t> index = channel_index(freq_hz);
    if (!index.has_value())
      return;
    Channel &channel = this->channels_[*index];
    const int16_t clamped = std::clamp(level_dbm, MIN_DBM, MAX_DBM);
    channel.bins[static_cast<size_t>(clamped - MIN_DBM)]++;
    channel.count++;
    if (channel.count >= QUIET_LEVEL_HALVING_READINGS)
      channel.halve();
  }

  /// Median of the readings of @p freq_hz, or nothing with fewer than QUIET_LEVEL_MIN_READINGS.
  [[nodiscard]] std::optional<int16_t> quiet_level_dbm(uint32_t freq_hz) const {
    const std::optional<size_t> index = channel_index(freq_hz);
    if (!index.has_value())
      return std::nullopt;
    const Channel &channel = this->channels_[*index];
    if (channel.count < QUIET_LEVEL_MIN_READINGS)
      return std::nullopt;
    const uint32_t middle = (channel.count + 1) / 2;
    uint32_t seen = 0;
    for (size_t bin = 0; bin < channel.bins.size(); bin++) {
      seen += channel.bins[bin];
      if (seen >= middle)
        return static_cast<int16_t>(MIN_DBM + static_cast<int16_t>(bin));
    }
    return std::nullopt;  // Unreachable: the bins add up to count.
  }

  /// The board's quiet level and the channel it was measured on, once every channel has enough
  /// readings; nothing before, so a board that has heard only part of its channels judges with the
  /// band's limit for that time (IoBand::busy_limit_until_quiet_level_dbm).
  [[nodiscard]] std::optional<ChannelLevel> board_quiet_level() const {
    std::optional<ChannelLevel> lowest;
    for (const uint32_t freq_hz : decisions::REQUEST_CHANNELS) {
      const std::optional<int16_t> level = this->quiet_level_dbm(freq_hz);
      if (!level.has_value())
        return std::nullopt;
      if (!lowest.has_value() || *level < lowest->level_dbm)
        lowest = ChannelLevel{freq_hz, *level};
    }
    return lowest;
  }

 private:
  static constexpr int16_t MIN_DBM = -130;  ///< Lowest bin; quieter readings count here.
  static constexpr int16_t MAX_DBM = -60;   ///< Highest bin; louder readings count here.

  /// The readings of one channel.
  struct Channel {
    std::array<uint16_t, MAX_DBM - MIN_DBM + 1> bins{};  ///< Readings per dB, from MIN_DBM up.
    uint16_t count{0};                                   ///< Sum of the bins.

    /// Halve every bin and recount, so the older half of the readings stops weighing in.
    void halve() {
      count = 0;
      for (uint16_t &bin : bins) {
        bin = static_cast<uint16_t>(bin / 2);
        count = static_cast<uint16_t>(count + bin);
      }
    }
  };

  [[nodiscard]] static std::optional<size_t> channel_index(uint32_t freq_hz) {
    for (size_t i = 0; i < decisions::REQUEST_CHANNELS.size(); i++) {
      if (decisions::REQUEST_CHANNELS[i] == freq_hz)
        return i;
    }
    return std::nullopt;
  }

  std::array<Channel, 3> channels_{};  ///< Indexed like decisions::REQUEST_CHANNELS.
};

/// @brief What is worth logging after the board's quiet level or its busy limit changed.
struct BusyLimitReport {
  std::optional<int16_t> limit_to_log_dbm;    ///< The limit was first learned or moved by a log step.
  std::optional<ChannelLevel> little_margin;  ///< The first time the quiet level leaves little margin.
};

/// @brief Decides when the learned limit is logged and when the noisy-board warning is, so the engine
/// only prints what this says.
class BusyLimitLog {
 public:
  /// Feed the board's current quiet level (nothing while it is not known).
  /// @return What to log now: the limit when it is first known or differs from the last logged one by
  ///         BUSY_LIMIT_LOG_STEP_DB or more, and the quiet level once per boot when it leaves
  ///         little margin below the upper bound.
  BusyLimitReport update(const std::optional<ChannelLevel> &quiet) {
    BusyLimitReport report;
    if (!quiet.has_value())
      return report;
    const int16_t limit = decisions::busy_limit_from_quiet_level_dbm(quiet->level_dbm);
    if (!this->logged_limit_dbm_.has_value() ||
        decisions::abs_difference(limit, *this->logged_limit_dbm_) >= BUSY_LIMIT_LOG_STEP_DB) {
      this->logged_limit_dbm_ = limit;
      report.limit_to_log_dbm = limit;
    }
    if (!this->warned_ && decisions::quiet_level_leaves_little_margin(quiet->level_dbm)) {
      this->warned_ = true;
      report.little_margin = quiet;
    }
    return report;
  }

 private:
  std::optional<int16_t> logged_limit_dbm_;  ///< The limit last reported.
  bool warned_{false};                       ///< The warning was reported once already.
};

}  // namespace home_io_control
}  // namespace esphome
