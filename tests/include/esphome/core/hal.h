#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "gpio.h"

namespace esphome {

/// Host-only recording of blocking delays. `delay()` cannot actually sleep here and `millis()` is
/// a fake counter, so timing-sensitive code would otherwise be untestable: a burst that forgot its
/// inter-frame gap entirely would look identical to one that honoured it. Recording the requested
/// durations lets a test assert the cadence that was *asked for*, which is the part this codebase
/// controls — the part it does not control is the scheduler's, and no host test can speak to that.
namespace test_hal {

inline std::vector<uint32_t> &recorded_delays() {
  static std::vector<uint32_t> delays;
  return delays;
}

inline void reset_delays() { recorded_delays().clear(); }

}  // namespace test_hal

/// The host clock behind `millis()`/`micros()`, in one of two modes.
///
/// **Legacy mode (the default)** is bit-for-bit what this stub has always done: two independent
/// per-call counters, monotonic across the whole test binary. It answers "did this call happen
/// after that one", never "how many milliseconds apart". Almost every existing test relies on
/// exactly this, so legacy mode must never change behaviour underneath them — see `peek_ms()` and
/// the "legacy drift" rule below.
///
/// **Manual mode** is a single `now_us` a test moves explicitly (`advance_ms()`/`advance_us()`/
/// `set_ms()`), with `millis()`/`micros()` both reading off it — one source of truth, so the two
/// can never disagree the way two independent counters could. It is opt-in per test
/// (`ManualClock`, RAII) because the radio chip drivers busy-poll `millis()`/`micros()` in a loop
/// with no other exit condition; under a clock that never moves on its own, that loop would spin
/// forever. Manual mode is for `MockRadio`-driven engine/hub tests, not driver tests — see the spin
/// guard below for what happens if that rule is broken.
namespace test_clock {

enum class Mode : uint8_t { kLegacy, kManual };

/// Production uses millis()==0 to mean "never happened yet" in several places (e.g. a device's
/// `last_seen_ms`), so a manual clock that started at 0 would make a fresh device's first update
/// indistinguishable from "no update yet". Starting comfortably above zero avoids that collision
/// without a test needing to know it.
constexpr uint32_t DEFAULT_MANUAL_START_MS = 100000;

/// A manual-mode busy-wait that never advances the clock hangs forever with no signal. This bound
/// converts that hang into a loud, immediate failure instead: high enough that no real bounded
/// loop between two `advance_*` calls should ever hit it, low enough that a genuine spin aborts in
/// well under a second. `hal.h` is included by production translation units, so it cannot pull in
/// gtest here — `std::abort()` is the only option, and a crashed test binary is still far better
/// than a hung CI job.
constexpr uint32_t SPIN_GUARD_READ_LIMIT = 1000000;

struct State {
  Mode mode{Mode::kLegacy};
  uint32_t legacy_ms{0};
  uint32_t legacy_us{0};
  uint64_t now_us{0};
  uint32_t reads_since_advance{0};
};

inline State &state() {
  static State s;
  return s;
}

inline bool is_manual() { return state().mode == Mode::kManual; }

/// Current time with no side effect, in either mode — for test infrastructure (e.g. `MockRadio`)
/// that needs "what time is it" without also being a `millis()`/`micros()` call that would move
/// the legacy counters. Infrastructure must always read time through this, never through
/// `millis()`/`micros()` directly: any new call to those from shared test code would shift every
/// existing legacy-mode test's timing.
inline uint32_t peek_ms() {
  State &s = state();
  return s.mode == Mode::kLegacy ? s.legacy_ms : static_cast<uint32_t>(s.now_us / 1000);
}

inline void note_read_() {
  State &s = state();
  if (s.mode != Mode::kManual)
    return;
  if (++s.reads_since_advance >= SPIN_GUARD_READ_LIMIT) {
    std::fprintf(stderr, "test_clock: manual time never advanced — a busy-wait loop is polling the clock; "
                         "use legacy mode for this test\n");
    std::abort();
  }
}

/// Manual only: moves `now_us` forward and resets the spin guard. Aborts in legacy mode — legacy's
/// counters advance only by being called, so "advance by N ms" has no meaning there and silently
/// accepting it would hide a test bug (a legacy-mode test that meant to use `ManualClock`).
///
/// A zero-length advance does *not* reset the guard: a caller that loops `delay(0)`/
/// `delayMicroseconds(0)` alongside a `millis()` poll (a real, if degenerate, busy-wait shape)
/// would otherwise reset the counter every iteration and defeat the very guard this is.
inline void advance_us(uint64_t us) {
  State &s = state();
  if (s.mode != Mode::kManual)
    std::abort();
  s.now_us += us;
  if (us > 0)
    s.reads_since_advance = 0;
}

/// `ms * 1000` is computed in 64-bit space so this stays correct for any `uint32_t ms` -- doing it
/// in 32-bit space would silently wrap above roughly 71.6 minutes (0x100000000 / 1000).
inline void advance_ms(uint32_t ms) { advance_us(static_cast<uint64_t>(ms) * 1000ull); }

/// Manual only: jumps to an absolute time, e.g. `set_ms(0xFFFFFF00)` to drive a wraparound test.
inline void set_ms(uint32_t ms) {
  State &s = state();
  if (s.mode != Mode::kManual)
    std::abort();
  s.now_us = static_cast<uint64_t>(ms) * 1000u;
  s.reads_since_advance = 0;
}

/// RAII switch to manual mode for the scope it's declared in, starting at `start_ms`. Restores
/// legacy mode on scope exit unconditionally (even if the test left the clock somewhere else), so
/// a `ManualClock` can never leak into a later test — the `test_isolation` listener also checks
/// this as a backstop, in case a test bypasses the guard some other way.
class ManualClock {
 public:
  explicit ManualClock(uint32_t start_ms = DEFAULT_MANUAL_START_MS) {
    State &s = state();
    if (s.mode == Mode::kManual) {
      // A second ManualClock's destructor would unconditionally force legacy mode on scope exit
      // (see ~ManualClock() below), silently dropping the outer one back to legacy mid-test rather
      // than restoring it -- loud and immediate beats a test that quietly stopped being manual.
      std::fprintf(stderr, "test_clock: nested ManualClock -- one is already active in this test; "
                           "only one may be alive at a time\n");
      std::abort();
    }
    s.mode = Mode::kManual;
    s.now_us = static_cast<uint64_t>(start_ms) * 1000u;
    s.reads_since_advance = 0;
  }
  ~ManualClock() {
    State &s = state();
    s.mode = Mode::kLegacy;
  }
  ManualClock(const ManualClock &) = delete;
  ManualClock &operator=(const ManualClock &) = delete;
};

}  // namespace test_clock

inline uint32_t micros() {
  test_clock::State &s = test_clock::state();
  test_clock::note_read_();
  if (s.mode == test_clock::Mode::kLegacy)
    return s.legacy_us++;
  return static_cast<uint32_t>(s.now_us);
}

inline uint32_t millis() {
  test_clock::State &s = test_clock::state();
  test_clock::note_read_();
  if (s.mode == test_clock::Mode::kLegacy)
    return ++s.legacy_ms;
  return static_cast<uint32_t>(s.now_us / 1000);
}

inline void delay(uint32_t ms) {
  test_hal::recorded_delays().push_back(ms);
  if (test_clock::is_manual())
    test_clock::advance_ms(ms);
}

inline void delayMicroseconds(uint32_t us) {
  if (test_clock::is_manual())
    test_clock::advance_us(us);
}

}  // namespace esphome
