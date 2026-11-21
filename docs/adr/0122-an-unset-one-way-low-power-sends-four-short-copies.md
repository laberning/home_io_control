# ADR 0122: An unset one-way `low_power:` sends four short copies for every manufacturer

<!-- doxygen-label: adr0122 -->

**Status:** Accepted, supersedes [ADR 0041](0041-unset-oneway-power-class-comes-from-the-manufacturer-profile.md)
and the default of [ADR 0119](0119-somfy-one-way-bursts-lead-with-one-wake-up-copy.md) · **Recorded:** 2026-11

## Context

A one-way identity without `low_power:` took its burst shape from its manufacturer's profile: four short copies for
VELUX, and for Somfy and every manufacturer without a profile one wake-up copy with the low-power flag followed by
three short copies (ADR 0041, ADR 0119).

A real Somfy Smoove remote, captured on the air, sends four short copies with no wake-up copy and no low-power flag
(`tests/corpus/captures/oneway/somfy_smoove_oneway_short_press_on_air.yaml`). An enrolled Somfy Izymo dimmer (mains)
obeyed four short copies 50 of 50 times from each of an SX1276, two SX1262 and an LR1121 board, with the copies
40 ms apart (ADR 0119) and again back to back (ADR 0121). The wake-up copy costs about 0.5 s of transmission per command on a
channel that allows 3.6 s an hour. Only a device that sleeps between commands, on battery or solar power, needs it.

## Decision

- An unset `low_power:` and `low_power: false` send four short copies, for every manufacturer.
- `low_power: true` sends one wake-up copy of `low_power_wake_ms` with the low-power flag, then short copies.
- The manufacturer profile no longer carries a default burst shape; one resolver turns `low_power:` into the shape.
- The docs say plainly that a battery or solar one-way device needs `low_power: true`.

## Consequences

- A command of the default shape keeps the transmitter on for about 0.07 s and holds the radio for about 0.1 s, so
  about fifty fit in the hourly budget of the one-way channel.
- A Somfy or unknown-manufacturer identity with `low_power:` unset changes on the air after an update: it no longer
  sends the wake-up copy. A battery or solar device configured that way misses commands until its identity gets
  `low_power: true`.
- A battery-powered one-way device was not on the bench; the wake-up shape for it rests on the Izymo runs of
  ADR 0119 and ADR 0121 and on the low-power flag the two-way protocol uses for the same purpose.
