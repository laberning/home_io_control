# ADR 0102: A device without a main entity is declared by a sensor entry, and its data-object readings run on a schedule

<!-- doxygen-label: adr0102 -->

**Status:** Accepted, amends [ADR 0099](0099-scheduled-transfer-reads-use-one-session-and-a-queued-reader.md) and [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md); amended by [ADR 0107](0107-a-covers-periodic-readings-and-battery-state-are-reading-rows-on-one-schedule.md) · **Recorded:** 2026-11

Builds on [ADR 0099](0099-scheduled-transfer-reads-use-one-session-and-a-queued-reader.md) (the queued
transfer reader) and [ADR 0100](0100-a-transfer-read-tells-an-accepted-request-without-an-answer-from-silence.md)
(how a read ends).

## Context

Some devices have values worth showing but nothing to control: a heat pump that keeps energy counters
in a data object is the first ([issue #244](https://github.com/laberning/home_io_control/issues/244)).
Every device in the configuration so far was declared by its main entity (a cover, a climate, a light),
which carries the `io_device_id`, the type and the readings. A device without such an entity had no place
to be declared.

A reading kept in a data object is read through the transfer reader, which takes seconds and gives way
to the hub's own radio work and to other controllers. It cannot be read on every status poll, and how a
read ends has to decide what the reading shows. The heat pump in issue #244 answers an open with nothing
to read about one time in three, and declines an open while another controller's transfer is running.

## Decision

1. **A `sensor:` entry declares a device that has no main entity.** It carries the `io_device_id`, the
   device type and the reading keys, like any other entry. ESPHome requires a sensor entry to hold an
   entity of its own; that entity is internal and never reaches Home Assistant. The readings are the
   entities the user sees.
2. **One main entity per device.** A device is declared by exactly one main entity, the sensor entry
   included. A second entry with the same `io_device_id` is a configuration error that names both.
3. **Data-object readings run on the periodic-poll schedule.** Each reading is scheduled per device: the
   first read a minute after boot, then at the entry's `update_interval`, or the reading's own default
   without one. An interval below five minutes is raised to five.
4. **How a scheduled read ends decides the reading:**
   - A value read: stored.
   - No value in the answer, no answer, a request the device challenged without answering, an empty open,
     an open the device declines with any other status (busy, another transfer open), an interrupted
     read: a missed read. The reading keeps its
     last value and is read again at the next interval. A read with no answer at all is asked once more
     after a minute before it counts, like every periodic poll; an interrupted read is retried soon and
     does not count as a miss.
   - The device saying it does not have the object, by an error reply to the open or by declining it
     with status 3 (as a Somfy Izymo does for objects it does not keep, see the read-object probe's
     results), or a recipe or offset the hub itself refuses: the reading is absent until the next boot, with one warning, and is not read again.
5. **Devices without a status read.** A capability marks device kinds that answer no status request; the
   hub never asks them for status, also not after overhearing them.

## Consequences

- A contributor adds a sensor-only device as a type-table row with its capabilities, a recipe, reading
  rows and their mirror rows; no YAML or hub code.
- A configuration that declared one device from two main entries is rejected; none of the documented or
  tested configurations did.
- A counter that a device sometimes cannot give keeps its last value in Home Assistant instead of
  disappearing. A device that never has the object stops being asked after one error reply.
- The reading table of [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md)
  gains the data-object source; values from every source live in one per-device store.

## Considered options

- **A hub-level block per device family** (a heat-pump block next to the radio settings): rejected, every
  device feature hangs off its device and its type-table row
  ([ADR 0059](0059-device-capabilities-come-from-the-type-table.md)).
- **Marking a reading absent on any empty or declined open:** rejected, the heat pump in issue #244 does
  both in normal operation, so its counters would disappear within hours.
- **Absent after several empty answers in a row:** rejected, it needs a count nobody has measured; an error
  reply is the device's own statement that the object does not exist.
