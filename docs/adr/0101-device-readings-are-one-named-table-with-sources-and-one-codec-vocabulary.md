# ADR 0101: Device readings are one named table with sources and one codec vocabulary

<!-- doxygen-label: adr0101 -->

**Status:** Accepted, amends [ADR 0083](0083-heating-and-hot-water-live-in-their-own-collaborator.md); amended by [ADR 0102](0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md), [ADR 0103](0103-every-heater-value-is-a-reading-kept-by-reading.md), [ADR 0104](0104-a-heaters-poll-reads-what-its-reading-rows-select.md), [ADR 0105](0105-every-device-value-write-is-judged-by-one-rule-per-register.md) and [ADR 0106](0106-a-water-heaters-values-are-readings-of-the-same-table.md) · **Recorded:** 2026-11

The heater's opt-in register list of ADR 0083 becomes the register sources of the reading table. Builds on
[ADR 0098](0098-device-capabilities-are-a-named-set.md): a reading names the capability a device needs.

## Context

A heater's energy counter, its occupancy and the minutes left of a towel dryer's run were each built as
heater-only entries: a list of opt-in registers with the capability each needs, a private encoding enum
inside the heater codec, three entity classes bound to a register by a device callback, and a Python
table in `climate.py` that mirrored the list. A cover's sun level and battery level are a third and
separate mechanism. A device that is not a heater (the Atlantic heat pump of issue #244 reports its
energy counters as data objects) would need a fourth mechanism with its own entity and codegen code, and
a second value-codec enum beside the heater's.

## Decision

1. **One table of named readings** (`DeviceReading`, `proto_device_readings.cpp`). Each row is what one
   entity shows: its name, the capability a device needs (a `DeviceFeature`), and its source.
2. **A source is a kind.** The kinds are alternatives of one type, so a new kind leaves the rows of the
   others alone. Two exist: a heater register (with an optional condition under which the register is
   read, such as the minutes left of a run only while that run is on) and a value derived from other
   readings (the minutes left of whichever run is active, with the registers it needs polled). Further
   kinds (a slice of a data object, a periodic one-frame read) are further alternatives.
3. **What is polled is derived from what is shown.** The registers the hub reads at every poll are the
   registers of the readings an entity declared; the heater's separate opt-in list is gone. A reading's
   capability gates its read, as before.
4. **One value-codec vocabulary** (`ValueCodec`, `proto_value_codecs.h`) with one pure decoder
   (`decode_value()`), shared by the heater register table and every later source. The decoder returns a
   number in natural units (`std::optional<float>`, so a counter wider than a signed 16-bit value and a
   temperature share it); the unit belongs to the entity.
5. **One lookup and one entity pair.** `reading_value(device, reading)` returns the value wherever its
   source keeps it. One numeric and one binary entity (`IOHomeDeviceReadingSensor`,
   `IOHomeDeviceReadingBinarySensor`) show any reading and publish only on change; the heater setting
   switch writes the register of its reading and shows it through the same lookup.
6. **The codegen mirror carries what C++ never needs.** `DEVICE_READINGS` in `hub_validators.py` lists
   each reading by name with its capability, source kind, polled registers, YAML key, entity kind and
   Home Assistant metadata. One helper in `platform_common.py` builds a main entity's reading keys,
   checks the capability and emits the code. A sync test compares names, capabilities, source kinds and
   registers with the C++ table.

## Consequences

- A new reading is a table row, its mirror entry and, at most, a codec: no new entity or codegen code.
- A user sees no change: the same keys under `climate:`, the same entities, names, units, device and
  state classes, and the same register reads per poll.
- Values stay where they are (`HeatingReadings`); a source that stores its value elsewhere adds its own
  store behind the same lookup.
- The cover's sun and battery readings and sensor-only devices are not on the table yet; the table
  leaves room for them.

## Considered options

- **Keep the heater opt-in list and add a second list for each new device family.** Rejected: every list
  repeats the capability gate, the codec and the entity wiring.
- **A second codec enum for data objects.** Rejected: the heater encodings and a data object's slices
  are the same few shapes; two vocabularies drift.
