# ADR 0118: The wake-up preamble defaults to 515 ms

<!-- doxygen-label: adr0118 -->

**Status:** Accepted, amends [ADR 0097](0097-every-preamble-is-set-as-a-duration-the-same-on-every-radio.md) and [ADR 0115](0115-a-low-power-one-way-wake-up-lasts-the-hubs-wake-up-duration.md) · **Recorded:** 2026-11

## Context

`low_power_wake_ms` is the hub's one setting for waking a resting device: the wake-up preamble of a two-way start
frame to a `low_power:` device believed at rest (ADR 0097, default 600 ms) and, since ADR 0115, the wake-up copy of a
`low_power: true` one-way identity.

A bench run on a Somfy Izymo dimmer (mains powered) with an enrolled one-way identity sent one command of each burst
shape, three times each. Four short copies and four 213 ms copies were obeyed every time. The `low_power: true` shape
(one wake-up copy, then three 10 ms copies) was obeyed with a wake-up of 300, 450 and 515 ms, and ignored every time
with 560 and 600 ms. So the 600 ms default made every `low_power: true` one-way command fail on that receiver.

The two-way evidence behind the wake-up length is resting battery devices that needed a 2048-byte wake-up on an
SX1262 board, about 427 ms (ADR 0095), where a resting VELUX SSL shutter answered 12 of 16 tries. ADR 0097 set 600 ms
because a VELUX KLI 110 remote sends about that much wake-up and all eight commands to two resting SSL shutters landed
on the first try with it; it left a shorter default such as 500 ms open until tested.

## Decision

- **`low_power_wake_ms` defaults to 515 ms**, for two-way and one-way alike. The range (300 to 1000 ms) is unchanged;
  the setting's step becomes 5 ms so that the default is a value of the number entity.

## Consequences

- A `low_power: true` one-way identity is obeyed by the Izymo with the default; the default stays above the 427 ms the
  two-way battery devices needed.
- A two-way exchange with a resting `low_power:` device starts about 85 ms sooner: three silent attempts take about
  3.2 s instead of 3.5 s, up to about 4.6 s when the device challenges every attempt, and one wake-up uses about 14 %
  of the hour the EU duty cycle allows on the request channel. A `low_power: true` one-way command holds the radio for
  about 0.72 s.
- A resting SSL is untested at 515 ms; the first-try rate seen at 600 ms is not shown for it. A user whose resting
  device needs a second press sets `600`.
- A user who set `low_power_wake_ms` keeps their value. A one-way receiver that needs a longer wake-up than the Izymo
  tolerates would need a hub of its own with a longer setting; none is known.

## Considered options

- **A separate one-way wake-up setting.** Rejected: one value serves both, and the bench gives no reason to split them.
- **Keep 600 ms.** Rejected: it makes every `low_power: true` one-way command fail on the Izymo.
