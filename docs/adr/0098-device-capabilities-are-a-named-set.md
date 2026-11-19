# ADR 0098: Device capabilities are a named set

<!-- doxygen-label: adr0098 -->

**Status:** Accepted, amends [ADR 0059](0059-device-capabilities-come-from-the-type-table.md) · **Recorded:** 2026-11

## Context

[ADR 0059](0059-device-capabilities-come-from-the-type-table.md) gave every row of the node-type table
one 16-bit capability word and mirrored it into the codegen as literal numbers: a `FEATURE_*` hex
constant per bit and a hex word per row in `hub_validators.py`, with the names of the set bits in a
comment beside it. Bit 15 of the Python word is not a capability but the row's experimental status,
packed into the word so that the sync test covers it.

Three things wear on this. The word fills up: the heater capabilities (window companions, night light,
central-heating mode, occupancy detection, the towel dryer's run timers) take one bit each, and the
fifteenth capability is the last that fits below the status bit, so every further one would need the
width decided again. The Python mirror is read through its comments, which nothing checks: a hex word
and the names beside it can disagree while the sync test passes. And a status that is not a capability
sits in the capability word.

## Decision

1. **A capability is a name, not a bit number.** C++ lists the capabilities in one enumeration; a row's
   capabilities are a set of those names (`DeviceFeatures`), and `has_feature()` asks the set. Adding a
   capability is one more enumerator; nobody chooses a bit position. The set's storage is an
   implementation detail with a compile-time check that every enumerator fits; it does not limit the
   number of capabilities a reader has to think about.
2. **Experimental is a row status, not a capability.** It stays the row's identity flag in C++ (as
   before) and leaves the capability set in the mirror: Python keeps a separate collection of the
   experimental kinds.
3. **The Python mirror names its capabilities.** Each row's entry in `hub_validators.py` is the set of
   capability names it carries, written as names; the codegen asks for a name. The sync test compares
   the names of every row and the experimental collection with the C++ table, so the mirror holds no
   number that could disagree with what it means.

Everything else in [ADR 0059](0059-device-capabilities-come-from-the-type-table.md) stands: one
capability set per row, one resolved copy per device, no branch on a type or subtype number, the
mirror checked row for row.

## Consequences

- A new capability touches the enumeration, the rows that carry it and the mirror, by name.
- A row with no capability is an empty set in both languages; the mirror keeps the rule of
  [ADR 0059](0059-device-capabilities-come-from-the-type-table.md) for which rows it lists.
- No configuration and no behaviour changes.

## Considered options

- **Widen the word to 32 bits, keep the status at bit 15.** Rejected: it postpones the next widening,
  leaves the status inside the capabilities and keeps the hex mirror.
- **Move the heater capabilities into the heating profile.** Rejected: "what can this device do" would
  then be answered in two places, which [ADR 0059](0059-device-capabilities-come-from-the-type-table.md)
  removed.
