# ADR 0071: A pulled key without an answer is proven by the devices that hold it

<!-- doxygen-label: adr0071 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0051](0051-pairing-reads-the-key-refusal-and-checks-an-unconfirmed-key.md) (the key pull:
"a different key must verify the device's 0x3D" and "an unproven key is not printed at all").

## Context

ADR 0051 accepts a pulled key only when the device that handed it over proves it with its 0x3D. A
Freebox Delta in its "Share your network" mode hands over its key when pulled, but answers none of
the hub's challenges (issue #215). Under ADR 0051 the attempt ended as `key_exchange_failed` and the
key was thrown away, although the reporter's own devices held it: seven of them answered a roll-call
under it.

The same report showed the device's frames arriving late. A 0x32 that answers an earlier 0x38 try
decrypts only with that try's request bytes, and a 0x3D that arrives in the next challenge's window
verifies only against the earlier challenge, so fresh random bytes per try turned late answers into
failures.

## Options considered

- **Keep ADR 0051's rule.** Rejected: the Freebox route then never yields a key, although the devices
  prove it.
- **Accept an unproven key without any check.** Rejected: a wrong decrypt would register devices under
  a key nobody holds and print it as if it were valid.
- **Prove an unanswered key by the devices that answer under it** (chosen). A device answers a
  roll-call only under the key it holds, so one answer proves the key as firmly as the giver's 0x3D.

## Decision

- When the device sends a key but no 0x3D comes back after its challenges, the hub runs the same
  search for key holders as after a proven key (the heater read or a roll-call, ADR 0058). If any
  device answers, the key counts as proven: the devices are registered and reported, and the key is
  printed once, as ADR 0051 describes for a proven key. If none answers, the key is not printed, the
  attempt ends as `key_exchange_failed`, and the advisor reports `key_pull_unproven`.
- A 0x3D that does not match is still final: the key is not printed and no roll-call runs.
- Every 0x38 try of one attempt carries the same six challenge bytes, and every verifying 0x3C repeats
  the same bytes, so a late 0x32 still decrypts and a late 0x3D still verifies.
- The first verifying 0x3C follows the 0x32 at once with the radio's response preamble; a repeated one
  follows a pause and uses the pairing start preamble, since the device may have left the channel.

## Consequences

- A pull from a device that never answers its challenge costs one more roll-call (about 13 seconds)
  before it fails.
- A key is printed only when a device proved it, by its own 0x3D or by answering under it; ADR 0051's
  safety rule holds in that form.
- Repeating the challenge bytes within one attempt reveals nothing new: the bytes are random per
  attempt, and the MAC of each answer still depends on the key.
