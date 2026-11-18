# ADR 0103: Every heater value is a reading, kept by reading

<!-- doxygen-label: adr0103 -->

**Status:** Accepted, amends [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md); amended by [ADR 0104](0104-a-heaters-poll-reads-what-its-reading-rows-select.md) · **Recorded:** 2026-11

Builds on [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md) (the
reading table, the shared codecs) and [ADR 0102](0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md)
(the per-device store of data-object values).

## Context

The reading table of ADR 0101 covered the heater's opt-in readings only. The rest of what the hub keeps
from a heater lived in a second structure: a field per register in `HeatingReadings`, reached through a
register-to-field table of its own, next to a third list that said which opt-in registers an entity had
asked for. The heating level and setpoint of a status reply were set by hand, outside both. Every
consumer (the climate card, the window sensor, the night light, the model check) read struct fields
directly, so a contributor adding a value had to learn two tables and two stores. ADR 0102 had already
put the values of data-object readings in a per-device store keyed by reading.

## Decision

1. **Every value the hub keeps from a heater is a reading.** Room temperature, setpoint, maximum
   setpoint, mode, regulator state, window state, towel mode, temporary state, working rate, the two
   temperatures in force, the derogated setpoint and the night light with its brightness are rows of the
   reading table, beside the opt-in ones. Each kept register is the source of exactly one row. Rows no
   entity of the YAML asks for have no capability and no key; the heater's profile decides which heaters
   read them.
2. **One store, by reading.** A device keeps every reading, from every source, in one per-device store
   (`IoDevice::reading_values`), indexed by reading and holding, per reading, a value, an "asked" flag
   and an "absent" flag. A value stored lifts "absent"; a reading found absent holds no value. A NaN is a
   value: a room-temperature sensor the heater reports as faulty is an answer. `HeatingReadings`, the
   register-to-field table and the opt-in read masks are gone. Storing by reading, not by register, is
   what lets the status values (which have no register) and the data-object values share the store.
3. **The status main parameter is a source kind.** A `StatusMainParameterSource` says whether the two
   bytes of a status reply are a heating level (the low byte under the profile's prefix) or a temperature
   over the device's range. They are decoded once with the shared big-endian codec. The heating level and
   the status setpoint are readings of this kind; the comfort setpoint register and the status setpoint
   are two readings, and the card picks by control path. A status reading is never polled by a register
   read, and a device with no status read never gets one.
4. **The register 0x0104 eco setback is no longer kept.** Nothing read it. It stays a register the
   `heating_control` action can read.
5. **Consumers read through `reading_value()`.** The card, the programme rules, the window sensor, the
   night light and the heating-level sensor take the device record and look readings up in the table. The
   rule "a read-back of the override sets the temperature in force" is a pure profile function, not part
   of the register read.
6. **The model check judges the towel-mode register by the answer.** A reply that carries the register's
   value bytes counts as a value, also when the codec refuses them; the register is what tells a towel
   dryer from a radiator.
7. **A re-pair keeps the whole store.** `DeviceRegistry::put()` copies the values and both flags to the
   record that replaces the device, so the card does not go blank after a re-pair and an entity keeps
   asking. The store is a RAM field of the device record and nothing is written to flash.

## Consequences

- A contributor adding a heater value adds one enumerator, one table row and one mirror entry; there is
  no second register map or field to touch.
- The YAML keys and entities stay the same, and so do the registers read per poll, their order and their
  cadence. Two edges change. After a re-pair the card keeps the values read before it instead of
  starting blank. A register the heater answered without a value, and that a later read (the
  `heating_control` action, a write's read-back) answers with a value, is polled again, because a stored
  value is proof the register exists.
- The poll still reads the registers of the heating profiles' lists. Each row already states when it is
  read, so a later change can derive the poll from the rows instead.
- A cover's sun level, rain and battery, a water heater's registers, the model-name register and the
  raw probes stay outside the table, each with its own mechanism.

## Considered options

- **Keep the values by register.** Rejected: the status values have no register, and the store of ADR 0102
  is already keyed by reading; two keys for one store would need a mapping in both directions.
- **A new capability for each register the card reads.** Rejected: which registers the card reads is a
  fact of the heating profile, which already names them; a capability would state it a second time.
- **Clear a re-paired device's store.** Rejected: it blanked the card after every pairing for no gain, and
  data-object values already survive a re-pair.
