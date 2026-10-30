#pragma once

/// @file radio_poll.h
/// @brief Preemption-safe "poll a condition until it holds or a deadline passes" helper.
/// @ingroup hioc_radio
///
/// Every blocking radio wait that samples a device and then compares a clock has the same hazard:
/// if the task is descheduled between the two reads for longer than the timeout (a Wi-Fi scan, a
/// flash write), the clock says "expired" while the device sample is already stale. Deciding on
/// that stale sample declares a ready device dead. poll_until() fixes the order once, for every
/// wait that must not misjudge a device on the strength of an old sample.

#include "esphome/core/hal.h"
#include "esphome/core/application.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// Outcome of @ref poll_until.
struct PollResult {
  bool ready{false};       ///< The condition held on the deciding sample.
  uint32_t elapsed_ms{0};  ///< Time from the first sample to the last clock read before the deciding one.
};

/// @brief Poll `ready()` until it is true or `timeout_ms` has passed, feeding the watchdog between samples.
///
/// The decision to give up is always taken on a sample read *after* the deadline was seen to pass,
/// so a task preempted for longer than the timeout can only make the wait longer; it can never make
/// a condition that became true look false. A caller can tell such an overrun from a clean success
/// by `ready && elapsed_ms > timeout_ms`.
///
/// The start time is the only clock read before the first sample, and each loop iteration reads the
/// clock once, so an already-satisfied condition costs exactly one `millis()` call (this runs before
/// every SPI transaction on the software-PHY chips, and host tests on the counting clock rely on it).
/// @tparam Ready Callable returning bool; true means the condition holds.
/// @param ready Condition to sample.
/// @param timeout_ms Window in milliseconds; the wait gives up once more than this has elapsed.
template<typename Ready> PollResult poll_until(Ready ready, uint32_t timeout_ms) {
  uint32_t const start = millis();
  if (ready())
    return {true, 0};
  while (true) {
    uint32_t const elapsed = millis() - start;
    if (elapsed > timeout_ms) {
      bool const last = ready();  // the deciding sample, taken after the deadline
      return {last, elapsed};
    }
    if (ready())
      return {true, elapsed};
    App.feed_wdt();
  }
}

}  // namespace home_io_control
}  // namespace esphome
