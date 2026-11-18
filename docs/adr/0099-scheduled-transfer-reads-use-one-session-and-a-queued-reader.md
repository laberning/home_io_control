# ADR 0099: Scheduled transfer reads use one session and a queued reader

<!-- doxygen-label: adr0099 -->

**Status:** Accepted, amends [ADR 0062](0062-a-read-only-data-transport-probe.md) · **Recorded:** 2026-11

## Context

[ADR 0062](0062-a-read-only-data-transport-probe.md) added the data-transport read as a diagnostic
probe: manual, one object per call, blocking the loop for the whole read, behind
`diagnostic_probes: true`. Some devices keep the values a user wants in such objects, for example the
energy counters of an Atlantic heat pump (issue #244), and a hub that shows them has to read them
regularly. A read is an open, a block request per 18 bytes and a close, and with the 650 ms pause
between requests (issue #156) even a short object takes seconds. Done in one go it would stall the API,
the covers and the other devices.

The same heat pump is read and written by other controllers, a gateway and room thermostats. It serves
one transfer at a time and keeps the handle of an opened one until it is closed, so a transfer of ours
that stays open makes it answer the others late or refuse them, and a thermostat then repeats its
write (issue #244). A hub that reads regularly has to get out of their way.

The probe's session logic (which request follows which answer, when to close) would otherwise be
written a second time for the scheduled read.

## Decision

1. **One session.** `TransferSession` walks one recipe at one offset: the open steps, the blocks
   counting down, the close. It is pure: it builds the next request and classifies each answer
   (read, empty, refused, malformed, too long, incomplete, no answer, abandoned). Once a device accepted
   an open, the last request is always the close. The `read_object` probe and the queued reader both
   drive it, so the order of requests and the meaning of each answer exist once.
2. **Recipes are the allowlist.** `TRANSFER_RECIPES` replaces the object table of ADR 0062. A row
   lists one or more opens (class, id, and whether the device is expected to answer that open empty),
   the offsets a read may be opened at (none listed means 0 only), the most bytes accepted, and
   whether an empty data open is tried once more from the first step. The offset is part of a read, not
   of the object: `create_transfer_open()` writes it big-endian into payload bytes 5 and 6 and builds
   only an open that a step of some row names, at an offset that row allows. The probe's 30 objects are
   the first 30 rows, one step each, offset 0, 432 bytes; their indexes do not change. Recipes for
   scheduled reads are appended behind them.
3. **`read_object` takes an offset.** The index is `<row>` or `<row>:<offset>`, the offset 0 when left
   out, and it must be one the row allows. `probe_sweep` on `read_object` walks the offsets of one row
   between two ends, at most 16 per sweep.
4. **Scheduled reads are allowed, read-only, for the objects of the table.** `TransferReader` queues
   reads, runs one at a time and does one exchange per step on one named timeout, so the loop runs between
   steps and a read never blocks. It needs no `diagnostic_probes: true`: what it can read is exactly what
   the table names, and nothing in it builds the write twin of the block. The probe stays behind
   `diagnostic_probes: true`.
5. **The reader gives way.**
   - While the hub holds the radio, a step retries 100 ms later. One predicate says when background radio
     work may run (no exchange in progress, no radio test mode, no key extraction awaiting its peer, no
     linked one-way remote transmitting); the operation queue's gate in `loop()` and the reader both use
     it.
   - A read starts only after 15 s without a data-transport frame (commands `0x46` to `0x4B`) of another
     controller to or from that device, tracked per device. A gateway session reads about 35 entries with
     gaps under a second, and a thermostat's write takes a second or two about every 93 s (issue #244).
   - Such a frame that appears while a read is open closes the transfer at once and ends the read as
     abandoned.
   - While a read of a device is in progress, the device's queued background operations stay in the queue,
     so a status poll cannot cut the transfer. A control operation is never held.
6. **The probe exchanges through the hub's path.** The `read_object` probe's exchanges count in the
   exchange counters and link health like every other exchange. The other probes keep exchanging through
   the engine directly, so their replies stay raw.

## Consequences

- Adding a readable object is a table row, and a device that needs two opens or an offset is a recipe,
  not hub code. Nothing in the hub queues a read yet: the first caller arrives with the first device that
  has a recipe.
- A device whose reads are scheduled is read in the loop between other work, so a read takes a few
  seconds more than the blocking probe and waits behind commands, pairing and another controller's
  traffic. A read that another controller interrupts is lost and has to be queued again.
- The 15 s quiet time is long next to a room thermostat's write, which a busy installation meets every
  few seconds; the reads then wait, which only delays them.
- The probe's silence message no longer tells an unanswered request from an acceptance without a reply
  for `read_object`; both say that no reply came.
- A recipe that is not read-only has no place in the table: writing stays excluded and has no builder
  (ADR 0062), and adding one is a new decision.
