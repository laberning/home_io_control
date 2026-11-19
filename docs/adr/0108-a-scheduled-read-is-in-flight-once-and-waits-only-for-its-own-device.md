# ADR 0108: A scheduled read is in flight once and waits only for its own device

<!-- doxygen-label: adr0108 -->

**Status:** Accepted, amends [ADR 0099](0099-scheduled-transfer-reads-use-one-session-and-a-queued-reader.md) and [ADR 0102](0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md) · **Recorded:** 2026-11

## Context

[ADR 0099](0099-scheduled-transfer-reads-use-one-session-and-a-queued-reader.md) gave the hub a queued
transfer reader that stays quiet for a device for 15 s after another controller's data-transport frames
to or from it, and [ADR 0102](0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md)
put the data-object readings on a per-reading schedule that hands each due reading to that reader.
Three things follow from the two together that neither record decides:

- The schedule advances when a reading is handed over, and the reader takes whatever it is given. A read
  that waits for a device to go quiet is still waiting when the reading is due again, so the same reading
  is queued a second time, and again each interval, until the queue is full. An installation where a
  thermostat writes to the device every few seconds (issue #244) keeps the read waiting for long.
- The quiet time is tracked per device, but the reader waits on the read at the front of its queue. A
  read for a device nobody else talks to waits behind one for a busy device.
- ADR 0099 says that once a device accepted an open, the last request is always the close. The reader
  looks the device up before each step, and a device that left the registry in between (a failed
  pairing's rollback removes the device it added) has no record to address the close to. The
  `read_object` probe runs its own session and does not look at a transfer the reader holds open to the
  same device.

## Decision

1. **One read in flight for each device and reading.** The hub notes a scheduled data-object reading as
   in flight when it hands the read to the reader and clears the note when the read ends, whatever the
   outcome, including a read the reader ends at once (refused, or abandoned for a full queue). A reading
   that is due while its read is in flight is skipped for that interval; the schedule keeps its cadence.
   The reader stays unaware of readings: it queues what it is given.
2. **A waiting read holds only its own device.** The reader starts the first queued read whose device has
   been quiet for the quiet time. When every queued read's device is held, it waits and chooses again. A
   read that has sent its first request is never reordered, and reads for devices that are not held run
   first in, first out.
3. **A device removed during a read is not closed.** The read ends as abandoned and the queue moves on.
   The transfer the device may hold open is left to the device: the hub's recorded exchange addresses
   registered devices only, and a second send path for a case that only a failed pairing's rollback
   reaches is not worth its risk. This is the one exception to "the last request is always the close".
4. **The probe refuses while the reader holds the device.** `read_object` for a device the reader has a
   transfer open to returns a message that says so and sends nothing. The probe blocks the loop while it
   runs, so the reader cannot start a step in between.

## Consequences

- A device another controller keeps busy costs one waiting read for each of its readings, not a growing
  queue, and the readings of other devices are read on time.
- A reading skipped because its read is in flight is read one interval later at the earliest; with the
  five-minute floor of ADR 0102 that is the price of a read that waited that long.
- Two readings that are slices of the same object are still two reads. Sharing one read between them is
  left until a device has such a pair.
- A removed device may keep a transfer handle until it drops it by itself. No capture shows how long a
  device keeps one; the case needs a pairing that fails after the device was registered while one of its
  reads is open.
- A user who calls `read_object` during a scheduled read of the same device is told to try again; the
  read takes a few seconds.

## Considered options

- **Dedupe in the reader, by device, recipe and offset.** Rejected: two readings of one object would
  collapse into one read whose answer only one of them receives, and the reader would have to know what
  a reading is.
- **Defer a due reading until its read ended instead of skipping the interval.** Rejected: the reading
  would be read twice in quick succession once the device goes quiet, for a value that is one read old.
- **Send the close to a removed device through the engine directly.** Rejected for now, see decision 3.
