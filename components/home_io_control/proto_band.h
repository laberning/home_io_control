#pragma once

/// @file proto_band.h
/// @brief The band a radio carries the protocol on: the air frequency of the three channels, the
/// channels a wake-up request may use, the busy limit used before a quiet level is known, and the
/// timing and default of a receiver on that band (reply windows, retry gap, low-power default).
/// @ingroup hioc_protocol
///
/// The protocol names its three channels by their 868 MHz frequencies (FREQ_CH1..3 in
/// proto_timing.h); those values are the channels' identity throughout the hub. A radio on another
/// band carries the same three channels on other air frequencies and says so through
/// RadioDriver::band(); everything above the driver reads this description and tests no band.

#include "proto_timing.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

/// The three channels of the protocol, identified by their 868 MHz frequency, in the order a channel
/// draw indexes them: channel 2 first, so a draw of 0 gives the channel every request used before
/// ADR 0096. IoBand::air_hz and ChannelSet are indexed the same way.
static constexpr std::array<uint32_t, 3> IO_CHANNELS = {FREQ_CH2, FREQ_CH1, FREQ_CH3};

/// @brief A subset of the three channels, a small value type over IO_CHANNELS.
class ChannelSet {
 public:
  /// All three channels.
  [[nodiscard]] static constexpr ChannelSet all() { return ChannelSet(ALL_BITS); }

  /// The set of the one channel @p freq_hz (empty when it is no channel).
  [[nodiscard]] static constexpr ChannelSet only(uint32_t freq_hz) { return ChannelSet(bit_of(freq_hz)); }

  /// Whether @p freq_hz is a channel of the set.
  [[nodiscard]] constexpr bool contains(uint32_t freq_hz) const { return (this->bits_ & bit_of(freq_hz)) != 0; }

  /// Number of channels in the set.
  [[nodiscard]] constexpr size_t count() const {
    size_t n = 0;
    for (size_t i = 0; i < IO_CHANNELS.size(); i++)
      n += (this->bits_ >> i) & 1U;
    return n;
  }

  /// The @p n-th channel of the set in IO_CHANNELS order (0-based); 0 when @p n is out of range.
  [[nodiscard]] constexpr uint32_t nth(size_t n) const {
    for (size_t i = 0; i < IO_CHANNELS.size(); i++) {
      if (((this->bits_ >> i) & 1U) != 0 && n-- == 0)
        return IO_CHANNELS[i];
    }
    return 0;
  }

  /// This set without the channel at index @p index of IO_CHANNELS.
  [[nodiscard]] constexpr ChannelSet without_index(size_t index) const {
    return ChannelSet(static_cast<uint8_t>(this->bits_ & ~(1U << index)));
  }

  [[nodiscard]] constexpr bool operator==(const ChannelSet &other) const { return this->bits_ == other.bits_; }

 private:
  static constexpr uint8_t ALL_BITS = 0x07;  // One bit per channel of IO_CHANNELS.

  explicit constexpr ChannelSet(uint8_t bits) : bits_(bits) {}

  [[nodiscard]] static constexpr uint8_t bit_of(uint32_t freq_hz) {
    for (size_t i = 0; i < IO_CHANNELS.size(); i++) {
      if (IO_CHANNELS[i] == freq_hz)
        return static_cast<uint8_t>(1U << i);
    }
    return 0;
  }

  uint8_t bits_;
};

/// @brief What the hub needs to know about the band a radio is on.
struct IoBand {
  /// The air frequency of each channel in Hz, indexed like IO_CHANNELS.
  std::array<uint32_t, 3> air_hz = IO_CHANNELS;
  /// The channels a request that must wake a low-power receiver may use.
  ChannelSet wake_up_channels = ChannelSet::only(FREQ_CH2);
  /// The RSSI at or above which a channel counts as busy while the board's quiet level is not known.
  int16_t busy_limit_until_quiet_level_dbm = CHANNEL_LIMIT_UNTIL_QUIET_LEVEL_DBM;
  /// How long a start frame listens for the first reply, in ms (the `exchange_start_response_wait_ms`
  /// key overrides it). See RESPONSE_START_WAIT_MS for the measurements behind the 868 MHz value.
  uint16_t start_reply_window_ms = static_cast<uint16_t>(RESPONSE_START_WAIT_MS);
  /// How long a read listens for a low-power receiver's reply, in ms (LOW_POWER_RESPONSE_WAIT_MS).
  uint16_t low_power_reply_window_ms = LOW_POWER_RESPONSE_WAIT_MS;
  /// The wait before each try after the first, in ms (EXCHANGE_RETRY_DELAY_MS).
  uint16_t retry_gap_ms = static_cast<uint16_t>(EXCHANGE_RETRY_DELAY_MS);
  /// Whether a device that does not declare `low_power` counts as battery-powered on this band.
  bool low_power_by_default = false;
  /// The time after a challenge beyond which the answer to it is reported as late, in ms; 0 reports
  /// none. A late answer means the receiver may already have stopped listening for it.
  uint16_t late_answer_warn_ms = 0;

  /// The frequency a channel really goes out on: the air frequency of @p channel_hz (a channel's
  /// identity, one of IO_CHANNELS), or @p channel_hz itself when it is no channel. Every line that
  /// prints a frequency prints this one.
  [[nodiscard]] constexpr uint32_t air_hz_of(uint32_t channel_hz) const {
    for (size_t i = 0; i < IO_CHANNELS.size(); i++) {
      if (IO_CHANNELS[i] == channel_hz)
        return this->air_hz[i];
    }
    return channel_hz;
  }
};

/// The 868 MHz band: the channels at their own frequencies, a wake-up request on channel 2 only, and
/// the fixed limit of ADR 0096, today's timing (proto_timing.h), no low-power default and no late-answer
/// warning. The default every driver returns from RadioDriver::band().
static constexpr IoBand IO_868_BAND{};

}  // namespace home_io_control
}  // namespace esphome
