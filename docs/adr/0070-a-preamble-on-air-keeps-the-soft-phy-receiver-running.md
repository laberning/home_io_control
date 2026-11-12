# ADR 0070: A preamble on air keeps the software-PHY receiver running

<!-- doxygen-label: adr0070 -->

**Status:** Accepted · **Recorded:** 2026-11

Applies to the SX1262 and LR1121 drivers (`SoftPhyDriverBase`). The SX1276 driver is unchanged.

## Context

Discover & Pair and the paired-device scan listen for single answers: a device answers a search once,
on one of the two channels the search did not use, at a random moment inside the answer window. The
listen loop (`ExchangeEngine::listen()`) hops between those channels in short dwells and lingers on a
channel when the radio reports a preamble.

On a Heltec V3 (SX1262) that loop missed about one answer in four. A microsecond trace of the listen,
lined up with an SDR recording of the same answers, showed why. In three of four misses the receiver
was on the right channel and had seen the preamble, and the frame was lost at a fixed deadline:

- every dwell or linger that timed out reset the receiver (a fresh `SetRx`), even with a preamble on
  air, and a reset within about a millisecond of the sync word lost the frame;
- the wait for the rest of a frame after its sync word was bounded by the same deadline, so a sync
  that landed at the end of a linger was abandoned mid-reception.

The fourth miss was a false preamble detection on the empty channel. With the SX1262's 8-bit
preamble detector the trace shows three to four detections per second of listening with nothing
behind them, each one holding the listen on that channel for 25-35 ms.

## Considered options

- **Longer linger only.** Tried on the bench (25 ms instead of 15 ms): it covers a longer preamble,
  but every timeout still resets the receiver, so a sync can still land just after a reset.
- **Longer answer preamble on the sender.** Tried on our own key-extraction responder (80 to
  200 bytes): within noise. Real devices choose their own preamble, so this was never a fix for them.
- **Keep the receiver running while a preamble is on air, give a started frame its air time, and
  lengthen the detector.** Chosen.

## Decision

1. **Hold.** When a dwell times out with PreambleDetected latched, the driver leaves RX and the
   sticky bit alone. `is_preamble_detected()` reads the live bit, so `listen()` keeps lingering on the
   channel. The hold ends when a frame arrives, when RX is reset or re-armed, on a retune, or after
   `SOFT_PHY_PREAMBLE_HOLD_MS` (30 ms), after which the timeout resets RX as before. This replaces the
   one-shot "preamble seen at the timeout" snapshot the driver used to keep.
2. **Post-sync grace.** Once the sync word is seen, the frame gets `SOFT_PHY_POST_SYNC_GRACE_MS`
   (the fixed-length probe's air time plus polling margin, 12 ms) past the caller's deadline.
3. **16-bit detector on the SX1262.** `SX1262_PREAMBLE_DETECTOR_16_BIT`, the value the LR1121 driver
   already uses. A real preamble runs 14 ms or more, so 16 bits costs it nothing.
4. The listen loop and `PREAMBLE_LINGER_DWELL_MS` stay as they are. With the driver holding RX, the
   linger length only sets how often the loop checks again.

## Consequences

- Measured on the bench, this change against the code before it, same session, blocks alternating,
  20 paired-device scans per arm (three paired devices answer, among them a Somfy Izymo):

  | Board | Requests to always-alive devices | Low-power requests |
  |---|---|---|
  | Heltec V3 (SX1262) | 64 % → 93 % of answers heard | 45 % → 64 % |
  | LilyGo T3-S3 (LR1121) | 69 % → 81 % | 55 % → 61 % |

  Izymo on/off control was 10 of 10 on both boards. A prototype of the same change caught our
  key-extraction responder's single discovery answers 20 of 20 times (79 % before), and Discover &
  Pair of the Izymo paired on the first search. The LR1121 gains less because it already had the
  16-bit detector; it still misses about one answer in five, for reasons not yet traced.
- A false detection now costs up to 30 ms on one channel instead of about 25 ms; with the 16-bit
  detector such detections were almost gone on the bench.
- A frame whose sync arrives can make a blocking receive return up to 12 ms after its deadline.
  Callers already budget much longer windows; none depends on the deadline to the millisecond.
- A 24-bit detector stopped all sync detection on the same bench, for a reason not yet known. The
  16-bit value is one step from that; do not raise it without measuring.
- The SX1276 path is untouched: its driver does not use `SoftPhyDriverBase`.
