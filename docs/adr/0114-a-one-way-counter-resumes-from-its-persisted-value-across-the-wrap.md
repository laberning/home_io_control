# ADR 0114: A one-way counter resumes from its persisted value across the wrap

<!-- doxygen-label: adr0114 -->

**Status:** Accepted, amends [ADR 0025](0025-persist-monotonic-counters-as-the-only-exception-to-0018.md) · **Recorded:** 2026-11

Amends [ADR 0025](0025-persist-monotonic-counters-as-the-only-exception-to-0018.md) (monotonic counters are the one
thing the hub persists): the rule for choosing between `initial_sequence:` and the persisted value at boot, and the
sentence about recovering from a wrap.

## Context

Each one-way identity keeps a 16-bit rolling sequence, persisted in blocks so a reboot never hands out a number twice
(ADR 0025). At boot the store chooses where to resume: the persisted value, or the identity's `initial_sequence:` from
YAML, which exists so a user can push a desynced counter forward.

The choice was a plain numeric comparison: the higher of the two. After the counter wraps past 65535 the persisted
value is small again, so an identity with, say, `initial_sequence: 60000` restarts at 60000 at the next boot, far
behind the number the device last accepted on the 16-bit ring. A device that remembers the last accepted number takes
the commands as replays and ignores them. The case is rare (65536 commands per identity) but silent.

ADR 0025 also says that a wrap is recovered "by the same resync path as any other desync". No such path exists in the
hub: there is no action that moves a counter, and what a device does with a sequence that jumps across the wrap has
not been measured.

## Decision

- **Boot rule.** `initial_sequence:` is used when nothing is persisted for the identity, or when it is above the
  persisted value by less than half the 16-bit range, compared as plain numbers:
  `persisted < initial_sequence < persisted + 0x8000`. Otherwise the persisted value stands. Forward stays the only
  direction YAML can move a counter, and a counter that has wrapped to a small value is not dragged back to an
  `initial_sequence:` half the range or more above it.
- The comparison is not ring arithmetic on purpose. On the ring a small value is "ahead" of every value more than half
  the range above it, so the default `initial_sequence: 0` would reset every counter past 32768 at the next boot.
- The rule is a pure function next to the store and is tested on the fast stage, including a reboot after a wrap.
- **Wrap recovery.** This record replaces ADR 0025's sentence: there is no resync action yet. After a wrap the
  counter carries on from the persisted value; whether a device accepts the jump from 65535 to 0 is not measured.

## Consequences

- A wrapped identity is no longer sent back to an `initial_sequence:` half the range or more above its counter.
- A wrapped identity whose counter is below its `initial_sequence:` by less than half the range still jumps forward to
  it once at the next boot. The numbers it lands on were last sent a lap earlier, so a device sees the jump as a large
  step forward on the 16-bit ring, not as a replay, and it can be far larger than a device accepts.
  Telling that case from a deliberate YAML edit needs state the store does not keep; it stays open.
- A user who sets `initial_sequence:` half the range or more above the persisted value sees it ignored, as a value
  at or below the persisted one is; the "Last 1W Command" sensor still shows the counter in use.
- Nothing changes on the air for an identity that has not wrapped and whose `initial_sequence:` is at or behind its
  persisted value.

## Considered options

- **Keep the plain comparison.** Rejected: after a wrap it sends the counter back to any larger `initial_sequence:`.
- **Compare on the 16-bit ring** (`initial_sequence - persisted`, wrapping, between 1 and 0x7FFF). Rejected: it resets
  every counter past 32768 to the default `initial_sequence: 0`.
- **Always prefer the persisted value.** Rejected: it removes the only way YAML can push a desynced counter forward.
