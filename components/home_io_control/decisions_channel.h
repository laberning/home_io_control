#pragma once

/// @file decisions_channel.h
/// @brief Pure channel-access decisions for the exchange engine's requests: which channel an attempt
/// uses, how long to back off after a busy check, when the access time is spent, which channel a
/// forced send takes (ADR 0096).
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing. Randomness is an argument:
/// the engine draws it from its injected source, a test passes a number.

#include "proto_band.h"
#include "proto_timing.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace esphome {
namespace home_io_control {
namespace decisions {

/// @brief How a request's transmit ended, as far as the retry plan and the wake belief care.
enum class ChannelAccessStatus : uint8_t {
  SENT,          ///< Sent on a channel the check found free.
  SENT_FORCED,   ///< Sent onto a channel measured busy (`lbt_force_when_busy`): no evidence about the receiver.
  CHANNEL_BUSY,  ///< Never sent: the access time ran out with the channel busy.
  FAILED,        ///< The radio could not send (serialization or TX failure).
};

/// The channels of the protocol in the order a channel draw indexes them (IO_CHANNELS).
static constexpr std::array<uint32_t, 3> REQUEST_CHANNELS = IO_CHANNELS;

/// A channel whose air frequency lies within this distance of the frequency the hub asks to keep away
/// from (its Wi-Fi channel) is left out of the channels of a request (ADR 0132): half a 20 MHz Wi-Fi
/// channel (11 MHz with the transmit mask's skirt) plus a margin. This project's choice, not a
/// measured value.
static constexpr uint32_t WIFI_CHANNEL_GUARD_HZ = 15000000;

/// Highest 2.4 GHz Wi-Fi channel on the regular 5 MHz raster (channel 14, Japan only, is off it).
static constexpr int32_t WIFI_LAST_REGULAR_CHANNEL = 13;
/// Wi-Fi channel 14, which sits off the raster.
static constexpr int32_t WIFI_CHANNEL_14 = 14;
/// Centre of Wi-Fi channel 0 on the raster in MHz; channel n is centred at this + WIFI_RASTER_STEP_MHZ * n (IEEE
/// 802.11).
static constexpr uint32_t WIFI_RASTER_ORIGIN_MHZ = 2407;
/// Spacing of the regular 2.4 GHz Wi-Fi channel centres in MHz.
static constexpr uint32_t WIFI_RASTER_STEP_MHZ = 5;
/// Centre of Wi-Fi channel 14 in Hz.
static constexpr uint32_t WIFI_CHANNEL_14_CENTRE_HZ = 2484000000U;
/// Hz per MHz.
static constexpr uint32_t HZ_PER_MHZ = 1000000U;

/// Back-off ceilings in milliseconds, one per busy check of an attempt; a later check than the table
/// holds uses the last one. A back-off is drawn uniformly from 1 to its ceiling, so the wait is
/// random and grows with every busy check. This project's design choice, not a measured value.
static constexpr std::array<uint8_t, 5> BACKOFF_CEILINGS_MS = {4, 8, 16, 32, 40};

/// The centre frequency of a 2.4 GHz Wi-Fi channel: 2407 MHz + 5 MHz per channel for 1..13, 2484 MHz for 14.
/// @param channel The primary Wi-Fi channel as the Wi-Fi stack reports it.
/// @return The centre in Hz, or 0 for 0 and any number outside 1..14 (not connected, 5 GHz, unknown); 0 means
///         "no Wi-Fi to keep away from" to request_channels().
[[nodiscard]] inline uint32_t wifi_channel_centre_hz(int32_t channel) {
  if (channel >= 1 && channel <= WIFI_LAST_REGULAR_CHANNEL)
    return (WIFI_RASTER_ORIGIN_MHZ + WIFI_RASTER_STEP_MHZ * static_cast<uint32_t>(channel)) * HZ_PER_MHZ;
  if (channel == WIFI_CHANNEL_14)
    return WIFI_CHANNEL_14_CENTRE_HZ;
  return 0;
}

/// Whether @p air_hz lies within WIFI_CHANNEL_GUARD_HZ of @p avoid_hz, the one definition of the guard (a 2.4 GHz
/// Wi-Fi centre is never that close to an 868 MHz air frequency, so no band check is needed).
/// @param air_hz The air frequency of a channel.
/// @param avoid_hz Air frequency to keep away from; 0 for none.
[[nodiscard]] inline bool inside_wifi_guard(uint32_t air_hz, uint32_t avoid_hz) {
  if (avoid_hz == 0)
    return false;
  const uint32_t distance_hz = air_hz > avoid_hz ? air_hz - avoid_hz : avoid_hz - air_hz;
  return distance_hz <= WIFI_CHANNEL_GUARD_HZ;
}

/// The channels a request may use (ADR 0132): all three; for a request to a low-power receiver the
/// band's wake-up channels; less every channel whose air frequency lies within WIFI_CHANNEL_GUARD_HZ of
/// @p avoid_hz. When that leaves no channel the exclusion is dropped.
/// @param low_power The request carries the low-power flag.
/// @param band The band the radio is on.
/// @param avoid_hz Air frequency to keep away from; 0 for none.
[[nodiscard]] inline ChannelSet request_channels(bool low_power, const IoBand &band, uint32_t avoid_hz) {
  const ChannelSet base = low_power ? band.wake_up_channels : ChannelSet::all();
  if (avoid_hz == 0)
    return base;
  ChannelSet narrowed = base;
  for (size_t i = 0; i < IO_CHANNELS.size(); i++) {
    if (inside_wifi_guard(band.air_hz[i], avoid_hz))
      narrowed = narrowed.without_index(i);
  }
  return narrowed.count() == 0 ? base : narrowed;
}

/// The channels of a band that requests give up because of the Wi-Fi channel the hub is on, and the Wi-Fi channels
/// that would give none up: what the boot hint tells a user.
struct WifiCoexistence {
  /// The air frequencies of the channels request_channels() leaves out for the Wi-Fi channel, in IO_CHANNELS order.
  std::array<uint32_t, 3> given_up_air_hz{};
  size_t given_up_count = 0;          ///< Entries of given_up_air_hz in use.
  bool has_clear_channel = false;     ///< Some Wi-Fi channel 1..13 would give none of them up.
  int32_t lowest_clear_channel = 0;   ///< The lowest such Wi-Fi channel; valid with has_clear_channel.
  int32_t highest_clear_channel = 0;  ///< The highest such Wi-Fi channel; valid with has_clear_channel.
};

/// What the hub's Wi-Fi channel takes from a band: the channels request_channels() drops for it (the rule the
/// requests follow, so the hint names exactly what the engine does) and the lowest and highest Wi-Fi channel that
/// would drop none of them. Nothing is given up (count 0) on a band the Wi-Fi is far from, 868 MHz included, or
/// without Wi-Fi.
/// @param band The band the radio is on.
/// @param avoid_hz The hub's Wi-Fi centre (wifi_channel_centre_hz()); 0 for none.
[[nodiscard]] inline WifiCoexistence wifi_coexistence(const IoBand &band, uint32_t avoid_hz) {
  WifiCoexistence result;
  const ChannelSet all = request_channels(false, band, 0);
  const ChannelSet kept = request_channels(false, band, avoid_hz);
  for (size_t i = 0; i < IO_CHANNELS.size(); i++) {
    if (all.contains(IO_CHANNELS[i]) && !kept.contains(IO_CHANNELS[i]))
      result.given_up_air_hz[result.given_up_count++] = band.air_hz[i];
  }
  if (result.given_up_count == 0)
    return result;
  for (int32_t channel = 1; channel <= WIFI_LAST_REGULAR_CHANNEL; channel++) {
    const uint32_t centre_hz = wifi_channel_centre_hz(channel);
    const auto covers_one = [&](uint32_t air_hz) { return inside_wifi_guard(air_hz, centre_hz); };
    if (std::any_of(result.given_up_air_hz.begin(), result.given_up_air_hz.begin() + result.given_up_count, covers_one))
      continue;
    if (!result.has_clear_channel)
      result.lowest_clear_channel = channel;
    result.highest_clear_channel = channel;
    result.has_clear_channel = true;
  }
  return result;
}

/// The channel for a check: a random one of @p channels other than @p avoid_freq.
/// @param channels The channels the request may use (request_channels()); must not be empty.
/// @param avoid_freq Channel to leave (the previous attempt's, or the one just measured busy); 0 or
///        any value that is not in the set excludes nothing. A set whose only channel is @p avoid_freq
///        returns that channel.
/// @param random Any random number; only its value modulo the number of candidates is used.
[[nodiscard]] inline uint32_t request_channel(const ChannelSet &channels, uint32_t avoid_freq, uint32_t random) {
  std::array<uint32_t, 3> candidates{};
  size_t count = 0;
  for (size_t i = 0; i < channels.count(); i++) {
    const uint32_t freq = channels.nth(i);
    if (freq != avoid_freq)
      candidates[count++] = freq;
  }
  return count == 0 ? channels.nth(0) : candidates[random % count];
}

/// The access time of one attempt: how long its checks may last before it ends as "channel busy".
/// @param low_power The request carries the low-power flag.
[[nodiscard]] inline uint32_t channel_access_budget_ms(bool low_power) {
  return low_power ? LOW_POWER_CHANNEL_ACCESS_BUDGET_MS : CHANNEL_ACCESS_BUDGET_MS;
}

/// The back-off ceiling of the @p busy_checks-th busy check (0-based).
[[nodiscard]] inline uint32_t backoff_ceiling_ms(uint8_t busy_checks) {
  return BACKOFF_CEILINGS_MS[busy_checks < BACKOFF_CEILINGS_MS.size() ? busy_checks : BACKOFF_CEILINGS_MS.size() - 1];
}

/// How long to wait before the next check after a busy one, or nothing when the access time is spent.
/// The wait never reaches past the end of the access time, so the last check of an attempt happens at
/// the limit, not after it.
/// @param elapsed_ms Time since the attempt's first check.
/// @param busy_checks Busy checks so far in this attempt (0 after the first).
/// @param low_power The request carries the low-power flag.
/// @param random Any random number, drawn fresh for this call.
/// @return The back-off in milliseconds (at least 1), or nullopt if the attempt ends as busy.
[[nodiscard]] inline std::optional<uint32_t> next_backoff_ms(uint32_t elapsed_ms, uint8_t busy_checks, bool low_power,
                                                             uint32_t random) {
  const uint32_t budget_ms = channel_access_budget_ms(low_power);
  if (elapsed_ms >= budget_ms)
    return std::nullopt;
  const uint32_t drawn_ms = 1 + (random % backoff_ceiling_ms(busy_checks));
  const uint32_t remaining_ms = budget_ms - elapsed_ms;
  return drawn_ms < remaining_ms ? drawn_ms : remaining_ms;
}

/// Whether a reading says the channel is busy: at or above the threshold, as the radio's own check
/// judges it.
[[nodiscard]] inline bool channel_is_busy(int16_t level_dbm, int16_t threshold_dbm) {
  return level_dbm >= threshold_dbm;
}

/// The limit a known quiet level gives: the quiet level plus QUIET_LEVEL_MARGIN_DB, held between
/// BUSY_LIMIT_FLOOR_DBM and BUSY_LIMIT_CEILING_DBM.
[[nodiscard]] inline int16_t busy_limit_from_quiet_level_dbm(int16_t quiet_level_dbm) {
  const auto limit = static_cast<int16_t>(quiet_level_dbm + QUIET_LEVEL_MARGIN_DB);
  return std::clamp(limit, BUSY_LIMIT_FLOOR_DBM, BUSY_LIMIT_CEILING_DBM);
}

/// The limit at or above which a reading makes a channel busy, judged against the board's own quiet
/// level (ADR 0096).
/// @param quiet_level_dbm The board's quiet level, or nothing while it is not known yet.
/// @param fallback_dbm The band's limit for the time without a quiet level
///        (IoBand::busy_limit_until_quiet_level_dbm).
/// @return @p fallback_dbm without a quiet level; else busy_limit_from_quiet_level_dbm().
[[nodiscard]] inline int16_t busy_limit_dbm(std::optional<int16_t> quiet_level_dbm, int16_t fallback_dbm) {
  if (!quiet_level_dbm.has_value())
    return fallback_dbm;
  return busy_limit_from_quiet_level_dbm(*quiet_level_dbm);
}

/// Whether a board's quiet level is so high that its limit has less than QUIET_LEVEL_WARN_HEADROOM_DB
/// of room below the upper bound: the highest of five readings at that level, plus the headroom,
/// would pass BUSY_LIMIT_CEILING_DBM.
[[nodiscard]] inline bool quiet_level_leaves_little_margin(int16_t quiet_level_dbm) {
  return quiet_level_dbm + QUIET_LEVEL_BURST_OFFSET_DB + QUIET_LEVEL_WARN_HEADROOM_DB > BUSY_LIMIT_CEILING_DBM;
}

/// The distance between two levels, in dB.
[[nodiscard]] inline int16_t abs_difference(int16_t a, int16_t b) {
  return static_cast<int16_t>(a > b ? a - b : b - a);
}

/// The channel a forced send takes once the access time is spent (`lbt_force_when_busy`).
/// @param channels The channels the request may use (request_channels()); must not be empty.
/// @param levels_dbm The level measured on each of REQUEST_CHANNELS, in that order; only the entries
///        of channels in the set are read.
/// @return The quietest channel of the set (the first of equals in REQUEST_CHANNELS order).
[[nodiscard]] inline uint32_t forced_send_channel(const ChannelSet &channels,
                                                  const std::array<int16_t, 3> &levels_dbm) {
  std::optional<size_t> quietest;
  for (size_t i = 0; i < levels_dbm.size(); i++) {
    if (!channels.contains(REQUEST_CHANNELS[i]))
      continue;
    if (!quietest.has_value() || levels_dbm[i] < levels_dbm[*quietest])
      quietest = i;
  }
  return quietest.has_value() ? REQUEST_CHANNELS[*quietest] : channels.nth(0);
}

/// Whether the request of a try went out on the air (on a free channel or forced); a try that did not
/// has nothing to listen for.
[[nodiscard]] inline bool request_was_sent(ChannelAccessStatus status) {
  return status == ChannelAccessStatus::SENT || status == ChannelAccessStatus::SENT_FORCED;
}

/// Whether a try that ended this way says anything about the receiver when it then got no answer. A
/// request that was never sent, or went out onto a measured-busy channel (where a collision explains
/// the silence), is no evidence that the receiver is at rest. A failed transmit keeps the rule that
/// predates channel access: the try counts.
[[nodiscard]] inline bool silence_is_evidence(ChannelAccessStatus status) {
  return status == ChannelAccessStatus::SENT || status == ChannelAccessStatus::FAILED;
}

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome
