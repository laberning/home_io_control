# ADR 0121: One-way copies go out back to back

<!-- doxygen-label: adr0121 -->

**Status:** Accepted, amends [ADR 0038](0038-oneway-bursts-follow-the-identity-power-class.md) · **Recorded:** 2026-11

## Context

Every one-way command goes out as four identical copies (ADR 0038). The hub waited 40 ms between them, so one burst
of four short copies held the radio for about 0.2 s, and copies arrived 47 to 51 ms apart on the air
(`tests/corpus/captures/oneway/unenrolled_window_opener_oneway_stop_short_burst_*.yaml`).

A real Somfy Smoove remote, captured on the air, sends its four copies back to back, 4 to 7 ms apart
(`tests/corpus/captures/oneway/somfy_smoove_oneway_short_press_on_air.yaml`). No device on record needs a longer
gap, and the channel check before every copy (ADR 0096) already keeps a copy off a busy channel.

## Decision

- The wait between copies is removed. A copy follows the previous one after the channel check and the radio's
  transmit setup only.
- The channel check before each copy stays unchanged.
- The copy count and spacing stay fixed; no setting is added.

## Consequences

- Measured with an SDR on the one-way channel, the next copy starts 6 to 13 ms after the previous one ends: about
  7 ms on an SX1276 board, about 10 ms on two SX1262 and an LR1121 board.
- A burst of four short copies holds the radio for about 0.1 s, one with the `low_power: true` wake-up copy for about
  0.6 s. The transmitter's on-time per command does not change, so the hourly budget of the one-way channel does not
  either.
- An enrolled Somfy Izymo dimmer (mains) obeyed 50 of 50 back-to-back commands with `low_power: true` and 50 of 50
  with `low_power: false` on each of the four boards, each command checked by a two-way read of the device's state and
  of the controller that last commanded it.
