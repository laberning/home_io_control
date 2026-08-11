# ADR 0024: Controller identities replace node addressing for 1W

## Status

Accepted

## Context

Every 2W entity in this component binds to a device by node address: `io_device_id: A1B2C3`
names one physical actuator, and the hub addresses frames to it.

One-way (1W) frames cannot work that way. A 1W command is sent to a **typed broadcast address**,
`(io_device_type << 6) | 0x3F` — "all awnings", "all lights" — and every device of that class in
radio range acts on it. Nothing on the wire names an individual device, and no reply comes back.
The addressing axis that 2W uses simply does not exist.

So a 1W entity has nothing to bind to on the device side. What actually distinguishes one 1W
control surface from another is on the *sending* side:

- the **source address** the hub transmits as, which each receiving device tracks a rolling
  sequence counter against;
- the **network key** the frame is authenticated with;
- the **device class** the frame is addressed to.

A further complication: those keys are not necessarily all the same. Adopting a foreign 1W
network's key (an existing remote's, recovered via the key-adoption feature) yields a key that is
not the hub's `system_key`, and it has to coexist with the hub's own network rather than replace
it. A single global key on the component cannot express that.

## Decision

Introduce **controller identities** (`oneway_controllers:`) as the 1W addressing model. Each
identity owns a source address, a network key, a device class, and its own rolling sequence
counter. 1W entities reference an identity by handle instead of naming a device.

- **`node_id` is optional and derived** from the hub's `node_id` plus the identity handle when
  omitted. Asking a user to invent a 3-byte radio address is an unanswerable question — nothing
  tells them which addresses are safe. Derivation happens at schema time, not on the device, so a
  derived address takes part in the same collision checks as a configured one.
- **`system_key` is optional and inherits the hub's** when omitted. Identities on your own
  network need not repeat it; an adopted foreign key is what makes the override necessary.
- **Addresses must be unique** across the hub's own `node_id` and every identity, enforced at
  compile time. Two transmitters sharing an address share a sequence counter, which desyncs both
  — and does so silently, since 1W has no replies to notice the failure with.

## Consequences

- A 1W entity has **no `io_device_id`**, and that is not an oversight — there is no such thing to
  bind to. It references a controller identity instead.
- **One hub holds several identities, possibly with different keys.** That is the point: it is
  what lets an adopted network sit alongside your own.
- Sequence-counter state is per identity, not per hub — see the sequence-store ADR.
- Two 1W entities sharing both an identity and a device class would be byte-identical on air.
  Rejecting that is deferred until entities exist.
- The identities ultimately belong to the transmit collaborator rather than to the component
  (ADR 0004); they start life on the component only because that collaborator lands later.
