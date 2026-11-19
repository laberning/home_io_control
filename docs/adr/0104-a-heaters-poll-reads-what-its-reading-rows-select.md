# ADR 0104: A heater's poll reads what its reading rows select

<!-- doxygen-label: adr0104 -->

**Status:** Accepted, amends [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md), [ADR 0083](0083-heating-and-hot-water-live-in-their-own-collaborator.md), [ADR 0103](0103-every-heater-value-is-a-reading-kept-by-reading.md), [ADR 0091](0091-every-climate-preset-is-a-command-and-the-heating-level-is-a-sensor.md) and [ADR 0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md); amended by [ADR 0106](0106-a-water-heaters-values-are-readings-of-the-same-table.md) · **Recorded:** 2026-11

Builds on [ADR 0103](0103-every-heater-value-is-a-reading-kept-by-reading.md): every value the hub keeps from a
heater is a row of the reading table, and each row already says what it needs. This record lets the row say when it
is read, and moves the Home Assistant details of a heater's entities into the same table.

## Context

After ADR 0103 the poll of a register heater still read the registers of lists that sat in the heating profile:
one list per kind of read (every poll, after the programme, may miss, read once, read when asked), a span field
for each, and a profile row of its own for the heater with a light, which differed from the radiator by its list
only. A contributor adding a polled value had to find the reading row, the profile lists and the entity code. On
the Python side the window sensor, the open-window-detection switch and the heating-level sensor were classes
and tables of their own in `climate.py`, with their names, device classes and IDs written there, while the other
readings were built from the mirror `DEVICE_READINGS`. The `heating_level:` key was accepted on every electrical
heater, although the hub reads a heating level from a plain heater and a towel dryer only; on a radiator, a heater
with a light and a pilot-wire interface, whose status carries no level (issue #228), the sensor never had a state.

## Decision

1. **Every reading row says when a heater's poll reads it.** A `ReadingPoll` field has five values:
   `EVERY_POLL` (the first read that fails ends the poll and fails it), `EVERY_POLL_MAY_MISS` (after the core
   reads; a miss keeps the value and costs a read, not the poll), `ONCE_AFTER_BOOT` (until the row holds a
   value), `WHEN_ASKED` (where an entity of the YAML asked for it, until the heater answers it empty) and
   `NOT_POLLED` (never by the poll walk: a status reply, the programme rule or a write's read-back delivers
   it). A data-object reading keeps its own schedule (ADR 0102) and a status row is never polled.
2. **A row applies to a heater by its capability or, without one, by the heating profile.** A row with a
   capability applies to a device that has it. A row without one applies where the profile's card reads that
   register, a pure function of fields the profile already has (`heating_profile_reads()`): the room
   temperature with `current_temperature`, the setpoint and its maximum with `target_temperature`, the register
   of `mode_fn`, of `preset_fn` where presets are observed and of the action source, and the registers of the
   programme. The poll and the card follow one fact; no capability is added per card register (ADR 0103).
3. **One selection function; table order is read order within a policy.** `heater_poll_reads()` walks the rows
   in table order and returns a small fixed-capacity list for one policy. The controller keeps the order of the
   phases of a poll (every-poll rows, the programme's registers, may-miss rows, asked rows, rows read once, the
   status level, the model check) and asks the table for each phase when it starts, so what an earlier phase
   read is current. The rows that an entity asks for are in the order of the register enumeration
   (`HeatingFunction`), which is the order the poll has always read them in; nothing sorts at run time. A row that derives from others (the minutes
   left of a run) names registers that are rows of their own, with the same policy and capability.
4. **The profile lists, their span fields and the heater-with-light profile are gone.** The registers a poll
   reads follow from the rows, the capabilities and the profile's own fields. `find_heating_profile()` already
   falls back from a model to its subtype's profile, so the heater with a light takes the radiator's profile and
   the capability `NIGHT_LIGHT` adds the light's registers.
5. **The mirror carries how an entity is declared.** A mirror row has a YAML `key` or a `companion`, never both.
   A companion has the suffix of its name, the suffix of its ID and, optionally, `disabled_by_default`; every
   device whose kind has the row's capability gets the entity, and its ID is declared at validation time as
   before (`inject_reading_companion_ids()`, `reading_companions_to_code()` in `platform_common.py`). The window
   sensor (the generic binary sensor with `device_class: window`) and the open-window-detection switch are
   companions of their readings. IDs, names and metadata are unchanged. The entity kind may also be a text
   sensor.
6. **A named reading carries its names in C++.** A row may have a `value_name` function; the heating level's is
   `heating_level_name()`. `reading_value_kind()` says whether a reading is a flag, a named value or a number,
   from the value-name function and the register's or data object's codec. One generic text sensor shows any
   named reading, with no state until a value is read and the last name kept for a value without a name. The
   window sensor and heating-level classes of the climate platform are deleted; `climate.py` keeps the
   temperature range and the night light, which stays its own entity class and key. A sync test checks every
   mirror entity kind against the C++ value kind.
7. **`heating_level:` follows a capability that is true exactly where a level is read.** The heating level is a
   key row of the mirror (a text sensor) gated by `REPORTS_HEATING_LEVEL`. The capability is on the plain
   electrical heater and the towel dryer, the kinds whose heating profile reports a level: a level-path profile
   whose status carries one (`status_reports_level`), or a programme whose period the status read reports. A
   pilot-wire interface is on the level path, but its status carries no level (issue #228), so its profile says
   so and its status main parameter is never taken as a level; the card shows the order last sent. A table test
   ties the capability to the profile, so there is one fact and a profile cannot gain or lose a level without the
   capability following. On a radiator, a heater with a light and a pilot-wire interface the key is a
   configuration error that tells the user to remove it. The sensor it removes never had a state there.

## Consequences

- A contributor adding a polled heater value adds the enumerator, the row (its policy, its capability) and the
  mirror row; there is no list to edit and no entity class to write. A new named value is a name function.
- The registers of a poll, their order and cadence are unchanged for every heater profile, shown by tests with
  the register sequences written out literally. The `WHEN_ASKED` rows are listed in register order, which is the
  order the poll read them in.
- Three edges change. A configuration with `heating_level:` on a radiator, a heater with a light or a pilot-wire
  interface stops validating and says what to remove. A level in a pilot-wire interface's status reply, which no
  interface has sent, would no longer move the card. The window sensor publishes a changed state only, like the
  other generic sensors, instead of on every update.
- The statements of ADR 0091 that the `heating_level:` sensor is accepted on every electrical heater and stays
  without a state on a radiator and a pilot-wire interface no longer apply: the key is rejected there, until such
  a heater is shown to report a level and gains the capability.
- ADR 0103's remark that the poll still reads the registers of the profile lists, and ADR 0055's that a profile names
  the registers a poll reads, are replaced by decisions 2 and 3.
  ADR 0101's derivation of the polled registers from the declared readings now goes through the poll policy, and
  ADR 0083's description of what `HeaterController` polls is the phase order of decision 3 over the table.
- A cover's rain, sun and battery values, a water heater's registers, the model-name register, the logged
  registers, the raw probes and the night light's entity class stay outside the table's entity mechanism, each
  with its own reason (see the [architecture overview](../architecture_overview.md)).

## Considered options

- **A per-register `update_interval`, like a data-object reading.** Rejected: a heater's registers are read on
  the heater's status poll and nothing a user sees would change; the policy only says whether a row is read on it.
- **Three policies (every poll, once, never).** Rejected: the "may miss" rows (a setting the heater's own
  controller changes, a part a model may lack) and the read-back-only registers behave differently and would
  have kept their lists.
- **Sorting the asked rows at run time.** Rejected: listing them in register order is free and the order is
  visible in the table.
- **A capability for each register the card reads.** Rejected: which registers the card reads is a fact of the
  heating profile, which names them; a capability would state it twice.
- **Keep `heating_level:` on every electrical heater, or accept it by a special validator in `climate.py`.**
  Rejected: the first keeps an entity that can never have a state, the second puts a rule the table knows back
  into the platform file.
- **Keep the window and level classes.** Rejected: they differ from the generic entities by a name, a device
  class and a text lookup, all of which the mirror and the row now carry.
