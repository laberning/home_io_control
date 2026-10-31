# ADR 0062: A read-only data-transport probe

<!-- doxygen-label: adr0062 -->

**Status:** Accepted · **Recorded:** 2026-10

Amends [ADR 0024](0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md): it
excluded `0x4A` from the probes, and this one sends it, as a read.

## Context

A Somfy controller reads two objects from every new actuator right after pairing, `0x8100` and
`0x8103`, and only then subscribes the events the actuator names in the first of them. Home IO
Control subscribes without reading either (ADR 0060), and an RS100 Solar io that accepted the
subscription to its sun-sensor event has sent no report on its own in a day (issue #156). The
object tells which events a motor can report, which sensor types it carries and on which radio
channels it listens; no public source gives its byte layout, so the only way to learn it is to
read it from a device.

ADR 0024 kept `0x4A` out of the probes because the reference documentation names it "Delete File"
and the block it belongs to (`0x46` to `0x4B`) contains writes. Two things changed. A decoded VELUX
copy session shows `0x4A` as a chunked read of a buffer that a `0x46`/`0x47` exchange opens, and the
source keeps its data. And the layouts of `0x46`, `0x47`, `0x4A` and `0x4B` are read from two
independent stacks, with the exact bytes a Kizbox sends for these objects reproduced in emulation of
its radio firmware.

## Decision

1. **`read_object` is a probe**, behind the existing gate: `diagnostic_probes: true`, a device already
   paired to this hub, and a refusal while the device is moving.
2. **Five objects, nothing else.** `index` 0 reads `0x8100`, 1 reads `0x8103`, 2 reads `0x8003`, a
   name record, which shows that the flow works before the other two are trusted, and 3 and 4 read
   `0x8001` and `0x8002`, the object form of the general info 1 and 2 answers. `create_transfer_open()` checks the id against that list itself, independent of the
   probe table. The index is never an on-air value.
3. **Read direction only, by construction.** `create_transfer_read()` builds exactly handle and chunk,
   three payload bytes, so a `0x4A` that carries data (the write direction) cannot be built from it.
   There is no builder for `0x48`, the opening of a write.
4. **One call, one session.** The probe opens the object, pulls its blocks counting down from
   `ceil(length / 18)`, and closes the transfer with chunk `00 00`. A transfer the device opened is
   closed again on every path, including after a failed block and when the announced length is above
   the 288-byte cap (16 blocks), in which case nothing is read.
5. **A block counts only if it echoes the request** (handle on its low seven bits, chunk in full), so a
   late answer to an earlier chunk is never spliced into the data.
6. **Raw bytes, never applied.** The data goes to the result (`response_hex`), to the log in short
   lines and to nothing else; it is not decoded and never reaches the device record. The decode comes
   after captures.
7. **Not sweepable.** The probe is a multi-frame session and the sweep machinery is one request per index.
8. **Plain request, as the vendor sends it.** The open carries the originator `SAAC` and is sent
   without a challenge. A device may challenge any request; the ordinary exchange answers. A Somfy
   Izymo accepted the open plainly and challenged every block read and the close.

## Consequences

- The `0x46`-`0x4B` block is no longer "never sent". Writing stays excluded and has no builder: adding
  one is a new decision.
- Some devices will refuse the open, answer `FE` or announce length 0; each is a result worth recording.
  What an actuator does with an unauthenticated read of these objects is not captured anywhere yet; the
  first run is on the bench.
- The byte layout of the objects stays unknown until captured. Any decoder, an entity built on it and
  automatic use of the event list are separate, later decisions.
- A transfer that fails halfway leaves a handle open until the close succeeds; the close is attempted
  with the normal retries, and a device that does not hear it times the handle out by itself (not
  verified).
