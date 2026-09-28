# ADR 0044: Chip-neutral transmit intent: wake level, idle scanning and air time

<!-- doxygen-label: adr0044 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

Every transmission goes through one choke point, `ExchangeEngine::transmit_frame()`, which hands
the driver a `RadioTxConfig`. Until now that config carried a channel and a preamble length, and
three decisions were left implicit in them:

- **How hard to wake the receiver.** The hub already decides this: a START frame to a duty-cycled
  receiver gets the 1024-byte `LONG_PREAMBLE`, any other START frame a short start preamble, and a
  frame that continues an exchange the driver's response preamble
  ([ADR 0029](0029-start-preamble-is-a-property-of-the-target.md),
  [ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md)). All three 868 MHz drivers
  wake receivers by preamble length, so the length *was* the decision. A radio that wakes receivers
  some other way would have to guess the level back from a length, and a threshold guess is
  fragile: the default `cold_broadcast_reply_preamble` is 80 bytes, so a "longer than 64 bytes"
  rule would treat every key-extraction broadcast reply as a wake-up.
- **Who scans the hop channels while idle.** `loop()` hopped from the host once per pass after
  `HOP_TIME_US`. That is the only option on the current chips, but a radio that can scan the
  channels by itself does it faster and more evenly than a loop that runs every 16–30 ms.
- **How long a transmission blocks.** The exchange budget (`exchange_total_budget_ms`, 2500 ms,
  [ADR 0013](0013-blocking-exchange-on-the-esphome-loop.md)) was checked against elapsed time only,
  before each retry. A try that starts just under the budget still spends its whole transmission
  and response window after it. On 868 MHz a long-preamble try transmits for about 220 ms, so the
  overshoot stays small; a radio whose wake-up takes 500 ms would let three unanswered tries run to
  about 3.2 s, past ESPHome's 2550 ms blocking warning.

## Options considered

- **Let each driver infer intent from the preamble length.** Rejected: the length does not carry
  the decision (see the 80-byte example above), and every driver would re-derive it.
- **Keep the three decisions in the hub, with a branch per radio.** Rejected: it breaks the rule
  that the controller layer talks to the radio only through chip-neutral `RadioDriver` members.
- **State each one once, in chip-neutral terms** (chosen).

## Decision

- **Wake level.** `RadioTxConfig` carries `TxWake wake` (`NONE`, `SHORT`, `LONG`: the receiver is
  already listening, awake but maybe scanning, or maybe duty-cycling). `transmit_frame()` sets it
  from `tx_wake_for(is_start(frame), preamble)` (`proto_timing.h`): `NONE` without START, `LONG`
  when the preamble is the wake-up burst (`is_wake_preamble()`, at least `LONG_PREAMBLE`), `SHORT`
  otherwise. That is right for every current preamble because every preamble tunable except
  `pairing_discovery_preamble` is capped below `LONG_PREAMBLE`, and that one is capped at it; a host
  test parses the tunable ceilings so a new tunable or a raised ceiling fails until its level is
  decided. Drivers that wake receivers by preamble length ignore the field.
- **Idle scanning.** `RadioDriver::run_idle_scan()` is called on every idle `loop()` pass. A driver
  whose radio scans the hop channels by itself starts or keeps that scan and returns true, and the
  hub does not hop from the host on that pass. Any explicit retune ends the scan. The default
  returns false, so the three current drivers keep host-side hopping. The key-extraction CH2 hold
  still runs first and wins.
- **Air time in the budget.** `RadioDriver::tx_air_time_us(len, cfg)` estimates how long
  `send_packet()` keeps the radio transmitting. It is pure virtual, so every driver states it; the
  three 868 MHz drivers return `io868_tx_air_time_us()` (preamble, sync word and the UART-coded
  frame at 38 400 b/s). The engine has one budget rule, `try_fits_budget_()`: a try starts only if
  `elapsed + gap + transmit time < exchange_total_budget_ms`. It guards both the ordinary retry
  (gap `EXCHANGE_RETRY_DELAY_MS`) and the re-send of an unconfirmed movement command (gap
  `UNCONFIRMED_EXECUTE_RESEND_DELAY_MS`,
  [ADR 0043](0043-an-unconfirmed-movement-command-is-resent-once-to-a-device-that-normally-confirms.md)).

## Consequences

- **A driver never has to infer intent**, and the one place the decision is made is the TX choke
  point every frame already passes, including 1W bursts, pairing and status acknowledgements.
- **On 868 MHz a retry is skipped slightly earlier.** It is now skipped when the elapsed time lies
  within one gap plus one transmission of the budget, instead of only once the budget is spent: at
  defaults, from about 2030 ms for a long-preamble try and about 2240 ms for a short one. Plain
  unanswered tries never reach that point (the check before a third long-preamble try runs at about
  1.5 s); only exchanges with a challenge and a final wait can, and such a try would have ended
  after the budget anyway. The timed corpus replays pass unchanged.
- **The re-send of an unconfirmed movement command is held to the same rule**, so it too needs room
  for its own transmission.
- **The estimate is a model, not a measurement.** For a 1024-byte preamble it gives about 218 ms of
  air time; the bench measurement per chip is still to be recorded, and the model is corrected if
  they differ by more than a few percent.
- **Every new driver must state its air time** (the virtual has no default), which is deliberate:
  the budget rule is only as honest as the estimate behind it.
