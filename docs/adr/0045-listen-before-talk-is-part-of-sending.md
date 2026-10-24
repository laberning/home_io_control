# ADR 0045: Listen-before-talk is part of sending

<!-- doxygen-label: adr0045 -->

**Status:** Accepted, amended 2026-10 (see [Amendment](#amendment-only-a-frame-that-opens-a-session-is-checked)) · **Recorded:** 2026-09

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

## Amendment: only a frame that opens a session is checked

**Recorded:** 2026-10. Issue #178: a wired Somfy OXIMO 50 io roller shutter on a Heltec V2
(SX1276). Across the reporter's four logs, every challenge response (`0x3D`) that listen-before-talk
held back by at least one back-off got no final reply (25 of 25), and every one sent without a
delay got one (33 of 33). A single 5 ms back-off was enough to lose the exchange. Back-offs before
the command itself did no harm: the shutter still answered with its challenge.

The device's window is device-specific. The corpus capture
`somfy_awning_exchange_open_lbt_contention_sx1276` shows a Somfy Sunea io awning answering a `0x3D`
that was held back by four to five back-offs. A strict receiver is enough to break the exchange,
on any chip, since the policy lives in `transmit_frame()`.

The check cannot help a frame that answers within a session. The peer has just transmitted on
this channel and is listening there for the answer, and after the last busy result the frame
goes out anyway. So the check can only delay the answer, possibly past the peer's window.

**Decision.** `transmit_frame()` checks the channel only for a frame with the CTRL0 START bit set,
the frame that opens a session. A frame without START goes out at once with no `cca_threshold_dbm`,
on the channel its caller names. The session's opening frame was checked by whoever sent it. The
frame builders already set START this way: every command, discovery, key-init and broadcast
request carries it, and the `0x3D`, an inbound `0x3C`, the key transfer `0x32` and the in-session
acknowledgements do not. The rule needs no per-call-site flag and no new tunable. It is the same
signal `tx_wake_for()` already reads ([ADR 0044](0044-chip-neutral-transmit-intent-wake-level-idle-scanning-and-air-time.md)).

**Consequences.**

- `lbt_max_retries` and `lbt_rssi_threshold_dbm` now govern only session-opening frames. Raising
  the threshold is no longer needed to keep a `0x3D` from being delayed.
- `lbt_retries` and the transmit observer's deferral events count only those frames.
- A busy opening frame still waits up to `lbt_max_retries` back-offs and is then sent anyway.
  Whether a request should instead fail, or move to another channel, is a separate decision. It
  needs the busy readings on the SX1276 after a strong received frame to be measured first.
