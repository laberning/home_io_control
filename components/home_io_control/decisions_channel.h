#pragma once

/// @file decisions_channel.h
/// @brief Pure channel-access decisions for the exchange engine's requests: which channel an attempt
/// uses, how long to back off after a busy check, when the access time is spent, which channel a
/// forced send takes (ADR 0096).
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing. Randomness is an argument:
/// the engine draws it from its injected source, a test passes a number.

#include "proto_timing.h"

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

/// The channels of the protocol in the order a channel draw indexes them: channel 2 first, so a draw
/// of 0 reproduces the channel every request used before ADR 0096.
static constexpr std::array<uint32_t, 3> REQUEST_CHANNELS = {FREQ_CH2, FREQ_CH1, FREQ_CH3};

/// Back-off ceilings in milliseconds, one per busy check of an attempt; a later check than the table
/// holds uses the last one. A back-off is drawn uniformly from 1 to its ceiling, so the wait is
/// random and grows with every busy check. This project's design choice, not a measured value.
static constexpr std::array<uint8_t, 5> BACKOFF_CEILINGS_MS = {4, 8, 16, 32, 40};

/// The channel for a check: a random one of the channels other than @p avoid_freq.
/// @param avoid_freq Channel to leave (the previous attempt's, or the one just measured busy); 0 or
///        any value that is not a channel excludes nothing, so the draw covers all three.
/// @param random Any random number; only its value modulo the number of candidates is used.
[[nodiscard]] inline uint32_t request_channel(uint32_t avoid_freq, uint32_t random) {
  std::array<uint32_t, 3> candidates{};
  size_t count = 0;
  for (const uint32_t freq : REQUEST_CHANNELS) {
    if (freq != avoid_freq)
      candidates[count++] = freq;
  }
  return candidates[random % count];
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

/// The channel a forced send takes once the access time is spent (`lbt_force_when_busy`).
/// @param low_power The request carries the low-power flag: it stays on its channel, the only one it
///        was ever allowed to use.
/// @param own_freq The channel a low-power request stays on.
/// @param levels_dbm The level measured on each of REQUEST_CHANNELS, in that order.
/// @return The quietest channel (the first of equals in REQUEST_CHANNELS order), or @p own_freq.
[[nodiscard]] inline uint32_t forced_send_channel(bool low_power, uint32_t own_freq,
                                                  const std::array<int16_t, 3> &levels_dbm) {
  if (low_power)
    return own_freq;
  size_t quietest = 0;
  for (size_t i = 1; i < levels_dbm.size(); i++) {
    if (levels_dbm[i] < levels_dbm[quietest])
      quietest = i;
  }
  return REQUEST_CHANNELS[quietest];
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
