# ADR 0123: A PROG window takes one pair or one unpair, and Enroll keeps remove-then-add

<!-- doxygen-label: adr0123 -->

**Status:** Accepted, amends [ADR 0026](0026-1w-enrollment-is-required-and-safety-comes-from-a-physical-interlock.md) · **Recorded:** 2026-11

## Context

The hub's Enroll button for a Somfy-profile one-way identity sends a remove-controller frame (`0x39`) and then an
add-controller frame (`0x30`), both to all devices. ADR 0026 described the remove as clearing only the identity's own
earlier entry before re-registering it.

A bench run on a Somfy Izymo dimmer in its programming mode showed what one programming window does:

- the first `0x30` from a controller the device does not know pairs it, and the device then follows from that frame's
  sequence number;
- the first `0x39` or `0x30` from a controller the device already knows removes it;
- a `0x39` from a controller the device does not know changes nothing;
- one window takes one such change.

The hub's own Enroll, remove then add, paired an identity the device did not know in one window: after a factory
reset of the dimmer, the next one-way command from that identity was obeyed and the device named it as the controller
that last commanded it.

So pressing Enroll for an identity the receiver already knows removes it instead of refreshing it. A real Somfy
Smoove remote's PROG press sends the same remove-then-add, captured on the air in
`tests/corpus/captures/enrollment/somfy_smoove_enrollment_prog_press_on_air.yaml`.

## Decision

- **Enroll keeps remove-then-add**, as a real remote sends it.
- The docs say that one programming window takes one pair or one unpair, and that Enroll on an identity the receiver
  already knows removes it; a desynced or lost identity is replaced by a new address enrolled fresh.
- ADR 0026's sentence that the remove "can only ever clear this identity's own prior entry before re-registering it"
  is replaced by this record.

## Consequences

- An identity is enrolled with one press in one programming window; a second press in another window removes it.
- Enrolling again is not a cure for a desync on a Somfy receiver.
- VELUX receivers are not measured.
