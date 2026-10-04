# ADR 0045: Listen-before-talk is part of sending

<!-- doxygen-label: adr0045 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

The 868 MHz band's listen-before-talk rule asks a transmitter to check that the channel it is
about to use is clear. The exchange engine did the check itself: before each frame it called
`read_rssi()`, retried up to `lbt_max_retries` times while the reading was at or above
`lbt_rssi_threshold_dbm`, then called `send_packet()`, which retuned to the frame's channel and
sent.

`read_rssi()` reads the channel the receiver is on. While the hub is idle the receiver hops over
all three channels, and every frame names its own channel, so for the first frame after idle
hopping the check usually measured a different channel from the one the frame then went out on.
A busy command channel could pass the check because the receiver sat on a quiet one, and a quiet
command channel could be delayed by traffic elsewhere.

A separate RSSI reading also assumes the host can read the radio's receiver directly and
immediately before sending, which not every radio arrangement allows.

## Options considered

- **Retune in the engine before `read_rssi()`.** Rejected: the engine would learn about settle
  times, which are chip properties, and the check would still be a separate step between the
  measurement and the transmission.
- **A separate "is the channel clear?" driver call before `send_packet()`.** Rejected for the same
  reason: two calls, a gap between them, and every driver would still need to retune in the first
  and remember it in the second.
- **Make the check part of the send** (chosen).

## Decision

- `RadioTxConfig` carries an optional `cca_threshold_dbm`. With it set, `send_packet()` measures the
  energy on `freq_hz` right before transmitting and sends only if it is below the threshold.
- `send_packet()` returns a `TxResult`: `SENT`, `CHANNEL_BUSY` (with the measured level, nothing
  sent) or `FAILED`.
- The three 868 MHz drivers share one helper, `RadioDriver::channel_busy_level_()`: if the receiver
  is on another channel it retunes to `freq_hz` and waits the chip's settle time
  (`SX1276_CCA_SETTLE_US`, 250 µs; `SOFT_PHY_CCA_SETTLE_US`, 1 ms), then reads `read_rssi()` there.
- `ExchangeEngine::transmit_frame()` keeps its policy: up to `lbt_max_retries` checked sends,
  `LBT_RETRY_DELAY_MS` apart, counting each busy result in `lbt_retries` and reporting it to the
  transmit observer; after the last busy result, one send without a check. A `FAILED` send ends at
  once.

## Consequences

- **The check measures the channel the frame is sent on.**
- **The first frame after idle hopping costs one retune plus the settle time**, at most about a
  millisecond, before the preamble starts. Frames that continue an exchange are already on their
  channel and pay nothing.
- **The settle times are datasheet-derived starting values.** The bench measurement per chip is
  still to be recorded, and the constants are corrected from it.
- **`read_rssi()` is no longer called by the controller layer**; drivers use it inside
  `send_packet()`, and it remains available for diagnostics.
- **A radio that can only check the channel as part of a send is covered by the same contract**,
  since the engine never asks for a separate reading.
