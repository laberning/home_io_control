# ADR 0081: A water heater's boost and away travel in the operating-mode word

<!-- doxygen-label: adr0081 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) (a domestic hot water device is
a water heater).

## Context

ADR 0068 shipped boost as a write of register `0x0000`. On the one heater reported so far (issue #198)
that write is acknowledged with the value `00` in every mode, while acknowledgements of writes the
heater keeps carry the written value. The heater ignores it, and the boost switch does nothing.

The same heater answers a status read of functional parameters 10, 15 and 16 (FPI1 `00`, FPI2 bitmap
`43`) with result code 0 and the main parameter in place, as `05 00 <MP> 00 43 00 00 00 0C 4A 00`
(issue #198, corpus `atlantic_calypso_probe_status_fp_high`):

- FP15 `000C` is the operating-mode capability word: bit 2 absence (away) and bit 3 relaunch (boost)
  are set.
- FP16 `4A00` is the current operating-mode word: relaunch in bits 8–9 and absence in bits 10–11, each
  `0` keep, `1` on, `2` off, so `4A00` reads "boost off, away off". Bit 14 is set; its meaning is not
  known.

The default status read (FP1–FP7) gets result code 5, "parameter unknown", because the heater holds
none of those parameters. Registers `0x0000` and `0x0005` change with a boost or holiday started on the
display: `01` during a boost, and in `0x0005` the holiday days still to run (`04` four days before the
end, `02` two days before) or `FF` for a holiday set with no end date (corpus
`atlantic_calypso_exchange_register_read`). They are durations, not on/off flags. During a holiday the
status main parameter reads `00 00` (corpus `atlantic_calypso_probe_status_mp_fp`).

## Decision

- **Boost and away are written in FP16** with one EXECUTE: main parameter `D400` (keep the target) and
  FP16 `4900` (boost on, away off), `4A00` (both off) or `4600` (away on, boost off). No other value is
  sent: there is no "both on", and the "keep" code is not used untested. Bit 14 stays set as the
  heater reports it.
- **A duration goes first.** Boost on writes register `0x0000` = 1 (a duration of 1; the unit is not
  known) and away on writes register `0x0005` = 255 (until turned off) before the FP16 EXECUTE. Both
  acknowledgements are logged; an ignored duration write does not stop the sequence. Off sends FP16
  `4A00` only, so ending one also ends the other.
- **The state is FP16.** The water heater poll's status read asks FPI1 `00` / FPI2 `43`, exactly the read
  the heater has answered: it returns the target, FP15 and FP16 in one exchange, plus FP10, which no
  decision uses and the decoder skips. Boost
  shows on while FP16 bits 8–9 are `1`, away while bits 10–11 are `1`. After each write the hub reads
  this status at once; the 45 s heating poll follows.
- **Registers `0x0000` and `0x0005` are durations.** The poll keeps reading them; they are logged and
  never decide a state.
- **Away is part of the water heater entity**, offered always (entity features are fixed at startup).
  Boost stays the opt-in switch. Once FP15 has been read, a write whose capability bit is clear is
  refused and logged.
- **No target while away.** The heater reports a main parameter of `0000` then, which is no target: the
  entity shows the target as unknown, and a target set while away is logged, not sent.
- **Both are experimental** until a heater confirms them on hardware.

## Consequences

- Boost and away from Home Assistant work on a heater that honours its operating-mode word; the poll
  costs no extra exchange.
- The whole poll (target, then the registers) rests on a read the heater has answered in three modes;
  FP10 costs two bytes per poll.
- FP16 has only been seen at rest. If it does not track a running boost, the shown state stays off
  while the heater boosts; a later record would then take the state from the durations.
- Ending a boost also ends a holiday and the other way round. Home Assistant's away mode has no end
  date, so away from Home Assistant runs until turned off; a holiday set on the display with an end
  date still ends on its own, and the entity follows.
- A heater that ignores the duration writes still gets the FP16 command; the logged acknowledgements
  show which part it honours.

## Considered options

- **Register writes only** (`0x0000` = 1, `0x0005` = 255): the boost write is ignored on hardware.
- **Registers as the state:** they carry durations, and a duration can outlive or precede the mode it
  belongs to.
- **Read-modify-write of FP16:** needs an extra read before each write and sends combinations no heater
  has been seen to accept.
- **FPI2 `03` (FP15 and FP16 only):** two bytes shorter, but never seen answered; a refused status read
  would stop every reading of the poll, not only boost and away.
- **The old FP1–FP7 read for the target plus a second read for FP15 and FP16:** keeps the target path
  unchanged, but costs a second exchange per poll.
