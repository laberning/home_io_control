# ADR 0107: A cover's periodic readings and battery state are reading rows on one schedule

<!-- doxygen-label: adr0107 -->

**Status:** Accepted, amends [ADR 0064](0064-a-sensor-reading-is-drawn-by-a-re-sent-event-subscription.md), [ADR 0088](0088-the-sun-level-is-shown-in-lux-at-100-lux-per-step.md), [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md), [ADR 0102](0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md) and [ADR 0103](0103-every-heater-value-is-a-reading-kept-by-reading.md) · **Recorded:** 2026-11

Builds on [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md) (the reading
table) and [ADR 0106](0106-a-water-heaters-values-are-readings-of-the-same-table.md) (status values as a source kind,
the platform of each mirror entry).

## Context

A cover's rain state, sun level and battery level were read by a table of periodic reads of their own, with their own
decoders, their own fields in the device record and their own sensor classes. The coarse battery state from byte 1 of
a status reply had two more sensor classes of its own. The data-object readings of
[ADR 0102](0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md)
already ran on a per-device, per-reading schedule that does the same work: wait while the device moves, ask once
more after silence, count misses.

## Decision

1. **Three more source kinds.** A reading can be the answer to an event subscription (the event, and a value form over
   its data), the answer to a limitation read (the rain flag), or the battery byte of a status reply. The sun level
   and the battery level are event readings, rain is a limitation reading, and the battery state is a status reading
   from which the low-battery flag is derived.
2. **One schedule for every scheduled reading.** Event, limitation and data-object readings share the per-device,
   per-reading schedule: the moving-device deferral, the one retry after silence and the miss counting. What differs
   between kinds (the request, the warnings for a refusal or an answer of another shape) belongs to the source kind;
   what differs between readings is one rule per row: after repeated misses, drop the value (rain, sun, battery) or
   keep it (a counter). At most one reading is due per pass of the loop, so a device with several scheduled
   readings never asks for two in one pass.
3. **Values are stored in their natural units.** The sun level is stored in lux, its estimated factor of
   [ADR 0088](0088-the-sun-level-is-shown-in-lux-at-100-lux-per-step.md) applied when decoding, as a scaled register
   value is stored in its own unit since
   [ADR 0105](0105-every-device-value-write-is-judged-by-one-rule-per-register.md).
4. **The generic reading entities show them.** The YAML keys that turn a reading on (the poll intervals, `battery`)
   stay; the mirror entry names the key that declares the reading and creates its entity.
5. **A state-change log line names the reading and its stored value**, for every scheduled reading. A status reply
   that shows no battery marks the battery state absent, and the hub logs that once per device.

## Consequences

- A cover reading is added like any other: a row of an existing source kind and its mirror entry.
- The device record keeps no rain, sun or battery fields; position, tilt and remaining time stay fields of the status
  frame and of the cover entity.
- YAML keys, entity names, IDs and metadata are unchanged. Three INFO lines read differently: the sun level in lux,
  the battery level without a unit, the rain state as 0 or 1. The warning, debug and configuration lines of these
  readings name them by their reading name (`sun_level`, `battery_level`, `rain`).

## Considered options

- **Keep the cover's own table of periodic reads:** rejected, it repeats the scheduling and the publishing that the
  data-object readings already have.
- **Apply the lux factor when the value is shown:** rejected, the generic sensor shows the stored value, and every
  other scaled value is stored in its natural unit.
- **A log label and unit on each row:** rejected, only the log would read them.
