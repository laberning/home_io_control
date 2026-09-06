# ADR 0025: Monotonic counters are the one thing the hub persists
<!-- doxygen-label: adr0025 -->

**Status:** Accepted · **Recorded:** 2026-08

## Context

[ADR 0018](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md) states
plainly that *"the component writes nothing to persistent storage."* One-way
(1W) transmit breaks that rule, and it cannot be built without breaking it.

1W is fire-and-forget: no challenge, no reply, no acknowledgement. Its entire
replay defence is a rolling 16-bit sequence number carried in each frame and
mixed into the authenticator. A receiving device remembers the highest sequence
it has accepted from each transmitter and rejects anything at or below it.

That makes the counter's high-water mark a piece of state we are obliged to
keep, and the consequence of losing it is severe and invisible:

- **Reuse a sequence** and the device rejects the command as a replay.
- **Fall behind** the device's high-water mark and *every* subsequent command is
  rejected, permanently, until the counter is manually resynced.
- **There is no error path.** 1W has no reply frame. A hub with a stale counter
  transmits perfectly-formed, correctly-signed frames that a device silently
  ignores. Nothing in the logs, the entity state, or the radio distinguishes
  this from a working installation whose device is out of range.

So the counter must survive a reboot. A power cut in the middle of a command
must not walk it backwards.

## Options considered

1. **Keep it in RAM and re-seed from YAML at boot.** Costs nothing and honours
   0018 literally. But a boot re-seed is exactly the failure above: after the
   first reboot the hub starts below the device's high-water mark and every
   command is silently dropped. This does not degrade — it stops working.
2. **Ask the user to paste the counter back**, as pairing asks for a device ID.
   Consistent with 0018's paste-and-reflash workflow, and absurd here: the value
   changes on every button press. It would mean reflashing after every command.
3. **Re-learn it from the air.** This is how the hub re-learns device types and
   names, and it is what 0018 means by "a cache, never a record". It is not
   available: in 1W the only party that knows the high-water mark is the device,
   and a 1W device never transmits. Nothing on air reports it.
4. **Persist the counter**, and nothing else.

## Decision

Option 4. `OneWaySequenceStore` (`oneway_sequence_store.h`) persists a
`uint16_t` per 1W controller identity via ESPHome's `ESPPreferences`, and is the
only place in the component that writes persistent storage.

The rule this establishes, and its boundary:

> **Persisted state is limited to monotonic counters that cannot be re-derived.
> Never configuration. Never a device record.**

Both clauses do work. A counter qualifies because it is (a) not configuration —
there is no sane YAML form for a value that changes on every command — and (b)
not a re-learnable cache, because nothing on air reports it. Device IDs, device
types, names, node IDs and system keys fail both tests and stay YAML-only under
0018, unchanged.

Two properties follow from the failure being asymmetric — **skipping a sequence
is safe, reusing one is not**:

- **Write before transmit.** The store persists the new high-water mark before
  it returns the value the caller will send, never after. A crash between the
  write and the transmit costs one skipped sequence; the reverse order would
  risk a reuse.
- **Reserve in blocks.** Writing flash on every button press is real wear, so
  the store reserves a block of sequences with one write and hands them out from
  RAM. An unclean reboot forfeits the unused remainder of the block, which is
  the safe direction. The block size is bounded by the device's forward-jump
  acceptance window and guarded by a `static_assert`, because a stride large
  enough to walk outside that window fails exactly like a stale counter.

## Consequences

- **0018 is no longer absolute**, and this ADR is the reason it isn't. An
  undocumented exception to an Accepted decision would be worse than the
  persistence itself: the next contributor would either propagate it or revert
  it, and both are wrong. 0018's consequences section cross-links here.
- **There is now flash state a user cannot see.** It is exactly the situation
  0018 was written to avoid, and the mitigation is narrow rather than complete:
  the persisted value is surfaced in the "Last 1W Command" diagnostic sensor, so
  a user can at least read the counter they cannot edit.
- **A migration story exists for the first time.** It is one `uint16_t` per
  identity, with a defined recovery — reseed it — so the cost is bounded, but
  "no migration story" is no longer something this component can claim.
- **Flash wear is bounded but nonzero.** One write per block of commands rather
  than one per command.
- **Replacing a board loses the counters.** Reflashing the same YAML no longer
  reproduces the installation exactly; the new board starts behind every
  device's high-water mark and needs a reseed. This is a genuine regression
  against 0018's "no hidden state to migrate or lose", and it is the price of
  1W transmit working at all.
- The counter is `uint16_t` and wraps after 65536 commands. A wrap looks to a
  device exactly like a large backward jump; recovery is the same resync path as
  any other desync.
