# ADR 0094: A towel dryer target in a comfort period changes the comfort temperature

<!-- doxygen-label: adr0094 -->

**Status:** Accepted, amends [ADR 0092](0092-a-target-set-in-auto-is-a-temporary-override.md) for the towel dryer · **Recorded:** 2026-11

## Context

[ADR 0092](0092-a-target-set-in-auto-is-a-temporary-override.md) makes a target set while a heater's
programme runs (`auto` on the card) a temporary override, written to register `0x011D` and read back, in
every period. Its Consequences name the case this record answers: if a heater refuses the override in a
comfort period, the rule for that heater kind returns to what its panel does.

On an Atlantic Adelis towel dryer in PROG
([issue #202](https://github.com/laberning/home_io_control/issues/202)):

- in an eco period the override is taken: the display shows it, it holds across polls and ends at the
  next comfort period, and writing 0 ends it early;
- in a comfort period the write is acknowledged, the register reads 0 right after and the dryer keeps its
  temperature. A target set then does nothing.

The dryer's own panel does the same split: in an eco period a change is temporary until the next comfort
period, in a comfort period it changes the comfort temperature.

The towel dryer answers the status read with its heating level, comfort or eco
([ADR 0091](0091-every-climate-preset-is-a-command-and-the-heating-level-is-a-sensor.md)). The value a
poll stored can be one or two polls old.

## Decision

- **On a towel dryer whose programme runs, a target in a comfort period is written to the comfort
  setpoint** (`0x0103`), as in `heat`. In an eco period it stays the override of ADR 0092, with its
  read-back.
- **The period comes from a status read made for the request**, in the same step that reads the mode and
  the timed run before a target (ADR 0092). A stored level is not used.
- **No period, no write.** If the status read gets no reply, or a level that is neither comfort nor eco
  (a boost that has just ended can still show as boost), nothing is written and a warning names the
  device, as for a heater that does not say what it runs.
- **The split is a property of the heater kind's profile**, not a branch on the device type. The radiator
  profile keeps ADR 0092 until a radiator's result decides its rule.
- **After a refused override the card shows the heater's own target at once**, not only at the next poll.

## Consequences

- A target set in `auto` always has an effect on the towel dryer: in a comfort period it changes the
  comfort temperature (and the eco temperature with it, minus the setback), in an eco period it holds
  until the next comfort period.
- The same gesture has a lasting effect in one period and a temporary one in the other, as on the dryer's
  panel. The docs say so.
- A target on the towel dryer in `auto` costs one more read before its write.
- The radiator family (AGILIA IO, Sauter Hekla, Thermor EMOTION 3 H) is unchanged. The EMOTION 3 H refuses
  the override as well ([issue #228](https://github.com/laberning/home_io_control/issues/228)); its rule
  waits for an eco-period result.

## Considered options

- **Keep the override in every period.** Rejected: in a comfort period a target does nothing.
- **Decide the period from the temperature in force against the comfort setpoint.** Rejected: it needs two
  registers and gives the wrong answer while an override runs.
- **Fall back to the comfort setpoint after a refused override.** Rejected as in ADR 0092: it would change
  the programme without saying so whenever an override fails for another reason.
