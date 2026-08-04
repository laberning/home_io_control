# ADR 0008: `io_device_id`, not `device_id` — and an `io_` prefix for protocol keys

**Status:** Accepted · **Recorded:** 2026-08

## Context

ESPHome's core config schema reserves `device_id` for its own sub-device
grouping feature — assigning an entity to a named sub-device of the node. This
component also needs a per-entity key naming which IO-Homecontrol device an
entity is bound to. The obvious short name is already taken, and taken by
something with entirely different semantics.

## Decision

Every device-bound platform uses `io_device_id`. The same `io_` prefix marks
the other keys naming protocol-level concepts that could otherwise be confused
with ESPHome's own vocabulary — `io_device_type` and `io_subtype` alongside it.

Keys that mean the same thing in both worlds keep their plain names
(`status_poll_interval`, `linked_remotes`); the prefix marks a collision or a
genuine ambiguity, it isn't decoration applied to everything.

## Consequences

- Slightly more verbose than the short names, in exchange for never colliding
  with — or being silently reinterpreted by — ESPHome's handling of
  `device_id`, both today and if a future release changes how it processes
  that reserved key.
- All of these keys are defined once in the shared platform module and
  extended into every device-bound platform's schema, so the naming cannot
  drift between cover, light, switch, and lock.
- A new option referring to "which IO-Homecontrol device" must reuse
  `io_device_id` rather than introducing a second name for the same concept.
- Because the key is required and validated centrally (exactly six hex
  characters), an entity that omits it fails at config-validation time with a
  clear message rather than misbehaving at runtime.
