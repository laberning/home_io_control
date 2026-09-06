# ADR 0029: The start preamble is a property of the target's power class, not of the frame's position
<!-- doxygen-label: adr0029 -->

**Status:** Accepted · **Recorded:** 2026-08

## Context

The first frame of every exchange this hub initiates went out with a 1024-byte
(`LONG_PREAMBLE`, ~213 ms) preamble, chosen by `exchange_engine.cpp` off
`is_start(request)` — a frame-position property. The intent, stated in
`proto_timing.h`, was a wake-up burst: a duty-cycled receiver needs a long
carrier in front of the frame or it never hears the sync word.

Two things were wrong with keying that off frame position:

- **Not every start frame is addressed to a sleeper.** An always-listening,
  mains-powered receiver does not need a wake-up burst, and a real controller
  does not send it one — a captured reference-hub `EXECUTE` to an
  always-alive device carries `CTRL1 = 0x00` and a short preamble, next to our
  byte-identical-payload `EXECUTE` carrying `CTRL1 = 0x20` and 1024 bytes.
- **Some receivers actively fail on the long burst.** Against a real Velux
  always-alive installation, every directed command timed out with the radio
  reporting it never detected a reply preamble at all. A bisection on the
  reporter's own hardware isolated the cause: clearing `CTRL1_LOW_POWER` while
  keeping the 1024-byte preamble changed nothing; dropping the preamble to a
  few bytes while *keeping* `CTRL1_LOW_POWER` set made every authenticated
  exchange complete. The long preamble — not the flag — was the blocker.
  Whether the receiver arms a bounded sync-search window that expires before
  our sync word arrives, or reads 213 ms of carrier as a busy channel, the
  emitted preamble is well-formed (Somfy always-alive devices lock onto the
  identical frames in volume) and simply too long for that receiver class.

The protocol already carries the distinction: every device that answers a
roll-call reports a `POWER_SAVE` class in its `0x2B` Multi Information Byte,
and `CTRL1_LOW_POWER` is a per-frame bit. `proto_constants.h` and
`proto_timing.h` both already described "always-alive → short preamble works /
low-power → needs the long preamble to wake" — the model was documented, just
never wired to the transmitter.

## Options considered

- **Preamble-only fix** (drop the directed start preamble globally, leave
  `CTRL1_LOW_POWER` as it was). The bisection shows the preamble is the whole
  fix, and the flag is historically tolerated by Somfy. Rejected as the
  shipped shape because it leaves the flag and the preamble expressing
  different ideas about the same device, and still treats a genuine
  solar/battery blind as always-alive.
- **Runtime application of the reported `0x2B` power class.** The wire tells
  us the class; apply it. Rejected here (not forever): nothing is persisted
  across boots (ADR 0018), and a hub that never runs a scan would never learn
  it — so it would be a rule that fires only in the sessions where a scan
  happened to run, which is non-deterministic behaviour for a property that
  decides whether a device answers at all. ADR 0019 reserves exactly this as a
  later, non-breaking fallback layer beneath the explicit option.
- **Reuse the per-chip `response_preamble()` for normal start frames.**
  Rejected: that value is a per-chip TX→RX turnaround property owned by the
  driver's `apply_tuning()`; a cold start frame to a hopping peer is a
  different situation, and conflating them means a future tune of one silently
  moves the other.

## Decision

The start preamble is `LONG_PREAMBLE` if and only if the frame carries
`CTRL1_LOW_POWER`, which is set if and only if the target's per-device
`low_power` property says so. That property is a YAML declaration (ADR
0018/0019), added once in the shared platform schema, defaulting to **false**
(always-alive — the protocol enum zero, the observed behaviour of the
majority of integrated devices).

- Every device-addressed builder takes an explicit `low_power` argument; none
  decides it internally. The call site reads it from the target `IoDevice`.
- One `ExchangeEngine` member, `request_preamble_for_()`, derives the preamble
  from the frame already in hand: non-start frames keep the chip's short
  response preamble; a start frame gets `LONG_PREAMBLE` when `CTRL1_LOW_POWER`
  is set and the runtime-tunable `normal_start_preamble` (32 bytes = 256 bits,
  inside the documented preamble band) otherwise. Both the directed exchange
  and the broadcast roll-call call it. The flag and the preamble can never
  disagree because both derive from the one property.
- The broadcast roll-call `0x2A` is built `low_power=false`, so it now goes out
  at `normal_start_preamble`. A broadcast has no per-device class to consult; a
  reference hub solves broadcast reachability by constant repetition, not
  preamble length.
- Pairing and roll-call decode the device's self-reported `POWER_SAVE` class
  and pre-fill `low_power: true` into the ready-to-paste YAML snippet when it
  is set. That is the override mechanism (ADR 0019's "learned default,
  explicit override"): the snippet proposes the value, the user edits the one
  line if the device mis-reports or a marginal link wants the long preamble
  forced on.

## Consequences

- **The protocol carries the power class and we deliberately do not apply it
  at runtime.** A solar or battery device left undeclared (or declared
  `low_power: false`) is treated as always-alive: its first frame per exchange
  may be missed while it sleeps, recovered by the ordinary retry, until the
  user pastes the `low_power: true` line the scan/pairing snippet already
  offered. This is the accepted wrong-answer cost of the `false` default, and
  it is milder than its opposite — a `true` default made every always-alive
  device 100% silent, which is the bug this ADR closes.
- **The `low_power == true` branch is unchanged and remains unvalidated
  against any real hub's own traffic.** `LONG_PREAMBLE` + `CTRL1_LOW_POWER`
  for a declared low-power target is exactly today's behaviour; there is no
  capture of a reference hub commanding a battery/solar device, so we keep the
  long preamble there on the documented-model argument, not on evidence.
- **`normal_start_preamble` is a live tuning knob.** 32 bytes is a defensible
  middle, not a measured number: 8 bytes is a proven floor against paired
  always-alive devices but nothing bounds where a start frame stops being
  heard, and brand-new devices have failed at 1/4/8 bytes on the soft-PHY
  chips (`docs/radio_diagnostics.md`). Shipping the number as a Home Assistant
  `number` entity lets a reporter bisect it live instead of rebuilding. One
  knob governs both the directed always-alive start frame and the roll-call
  broadcast preamble, so a user who needs a longer broadcast preamble to reach
  a sleeping device cannot lengthen it without also lengthening every directed
  frame — a follow-up candidate if roll-call reliability regresses.
- **Pairing is out of scope and carries the same latent problem.** The
  pairing-phase-3 config frame (`create_set_config1`) is a directed 2W start
  frame that still transmits at `LONG_PREAMBLE`, so a Velux always-alive
  device probably cannot be *paired* by this codebase either, for the same
  reason it could not be commanded before this change. Pairing frames are
  separately hardware-validated at `LONG_PREAMBLE` and pairing was never
  implicated in the issue, so this is a real follow-up, not this change.
- **The next "every directed frame is low-power" assumption has nowhere to
  hide.** `CTRL1_LOW_POWER` and the start preamble are now one derived pair
  keyed off a single per-device property; a builder that hardcodes the flag,
  or an exchange site that keys the preamble off `is_start()` alone, is
  recognisable on sight as the mistake this ADR records.
- Structurally this is the twin of ADR 0028: a radio decision that was being
  keyed off the wrong property (there, the chip; here, the frame's position)
  moves to the property that actually governs it (the target device's class).
