# ADR 0105: Every device value write is judged by one rule per register

<!-- doxygen-label: adr0105 -->

**Status:** Accepted, amends [ADR 0054](0054-heaters-are-read-back-over-the-private-register-protocol.md), [ADR 0057](0057-temperature-setpoints-travel-in-the-main-parameter.md) and [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md) · **Recorded:** 2026-11

Builds on [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md) (the shared
codecs) and [ADR 0054](0054-heaters-are-read-back-over-the-private-register-protocol.md) (one register table drives
get, set and decode).

## Context

The bounds and allowed values of a heater write lived in several places: the register encoder, the `heating_control`
action (its own function names, mode list, presence and detection words and temperature bounds), the climate card's
maximum-setpoint guard, the in-force rule, the night light (two scale helpers and its own clamps) and the main-parameter
temperature encoder, which clamped where every register refused. A contributor changing what a register accepts had to
find all of them, and the action could not say what the encoder would do with a value. The codecs of ADR 0101 decoded
only, so each writer kept its own byte layout.

## Decision

1. **The codecs encode as well as decode.** `encode_value()` is the inverse of `decode_value()` over a `ValueForm`
   (codec, bound, write range, scale). It refuses a value the register does not take, never rounds it into range or
   truncates it, and writes the bytes in the order the register reads them back in.
2. **A register's write rule is its row.** A row of the heater register table carries the codec and bound, a write
   range (the tenths registers: the setpoint and the derogated setpoint, 7.0 to 28.0 degrees), an optional named
   refusal predicate (the mode register refuses the external-setpoint mode) and, for a register shown on a scale of
   its own, the scale. `heating_write_accepts()` is the one judgement and is exactly what the encoder encodes;
   `heating_write_range()` gives the range a card or a message states. No caller repeats a bound, a value list or a
   function name.
3. **The action looks functions up by name in the table.** The rows the `heating_control` action offers (the same eight
   functions as before) carry a label for the value in its messages. The action parses the value by the register's
   codec, judges it by `heating_write_accepts()` before any device check and builds its messages from the label and the
   range. Mode words live beside `HeatingMode`, and only writable modes are offered. A value that equals zero cancels
   an override (`0.0` and `-0` too), because the codec of the derogated setpoint takes zero.
4. **A scaled form.** `SCALED_U16` maps a 16-bit register to natural units over ascending knots, linear between two.
   The night light's brightness register carries the heater app's scale, so its reading holds a percentage, the light
   writes and shows a percentage, and the two scale helpers are gone. The light's rule that a lit light is at least
   1 % stays the light's.
5. **A main-parameter temperature outside its range is refused, not clamped.** `encode_temperature_main_parameter()`
   refuses a temperature outside `[min, max]`, `plan_heating_write()` answers `OUT_OF_RANGE`, and the climate entity logs
   a warning. The card of a main-parameter heater never offers what the device cannot take: it offers the part of the
   profile's 5 to 30 degrees that lies inside the device's encoding range (the YAML `min_temperature:` and
   `max_temperature:` on a thermostat, the fixed range on a heat pump); the encoder is the second guard.
6. **A register preset writes the profile's preset register.** The per-preset write function is removed: a register
   profile names the register its presets write once (`preset_fn`); the presets of the level and alias profiles are
   not register writes and read no register from their rows.

## Consequences

- A new writable register states its codec, range, refusal and label in one row; the [contributing guide](../contributing.md) describes it.
- The brightness reading is a percentage; a value above the register's maximum is still no reading.
- A thermostat with the default range (-40 to 80 degrees) or a YAML range that holds 5 to 30 offers 5 to 30 as
  before; a narrower YAML range narrows its card.
- A target of a thermostat outside its YAML range is no longer sent as the end of the range.
- Placeholder register defaults remain on the profiles that are not register profiles (`HeatingWritePlan::register_fn`,
  `read_back_fn`, the profile's `mode_fn` and `preset_fn`); making them optional is left to a follow-up.
- The water heater's writes still use their own encoders and move onto these rules separately.

## Considered options

- **Keep the clamp of the main-parameter temperature as the one exception.** Rejected: a target the user typed would
  go out as a different temperature, and the rule would need an exception to explain.
- **Expose every writable register in the action.** Rejected here: widening what the action can write is its own
  decision; the action keeps its eight functions, marked in the table.
- **One global temperature bound.** Rejected: the registers and the main parameter have different ranges, and a global
  bound is the repeated number this record removes.
- **Keep the brightness scale in the light.** Rejected: the write and the read would use two statements of one rule.
