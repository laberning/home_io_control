# ADR 0063: A read-only register probe

<!-- doxygen-label: adr0063 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0024](0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md): it left
`CMD_WRITE_PRIVATE` (`0x20`) out of the probes, and this one sends it, as a read.

## Context

Register-protocol heaters (an Atlantic AGILIA IO radiator, for one) are driven with `0x20` frames
`0C <direction> <register high> <register low> [value]`, where direction `60` reads and `61` writes
([ADR 0054](0054-heaters-are-read-back-over-the-private-register-protocol.md)). Climate support sends
the read direction to those radiators on every poll (issue #165).

An Atlantic Calypso Connecté water heater (node type `0x33`) pairs with Discover & Pair and accepts the
key push (issue #198). Which registers it serves is unknown, and the hub has no entity for the type.
The only way to find out is to read candidate registers and record what comes back, which the existing
heating codec cannot do: it knows ten named registers, all in the `0x01xx` family.

ADR 0024 kept `CMD_WRITE_PRIVATE` out of the probes because a wrong write to a device's stored
configuration cannot be undone from the radio side. A read has no such effect.

## Decision

1. **Two probes**, behind the existing gate (`diagnostic_probes: true`, a device already paired to this
   hub, a refusal while the device is moving):

   | probe | request payload | `index` |
   |---|---|---|
   | `register_00` | `0C 60 00 <index>` | low byte of register `0x00xx` |
   | `register_01` | `0C 60 01 <index>` | low byte of register `0x01xx` |

   Family `0x01xx` is the one climate support already reads. Family `0x00xx` is untested.
2. **Read direction only, by construction.** One builder, `encode_heating_register_read()`, frames the
   request. It takes a register number and no direction: the direction byte is the read constant, so no
   input, index or caller can produce `61`. A test runs every register number through it, and another
   runs every index of both probes through the hub and checks the byte on the wire.
3. **A device-kind gate.** The probes are refused, with a terminal refusal that also ends a sweep, unless
   the device is a register-protocol heater (`device_uses_heating_registers()`) or a domestic hot water
   device. `IoDevice` has no manufacturer field, so the gate reads the kind, not the vendor. It gates
   the probes only; register writes stay behind `device_uses_heating_registers()`.
4. **Sweepable.** A read changes nothing, so `probe_sweep` accepts both probes, with its 16-index cap.
5. **Same exchange as the other probes.** The request goes through the ordinary authenticated exchange
   on channel 2. Like every probe it does not record link health or result codes on the device the way
   a heating poll does.
6. **Raw output.** The result carries the raw reply. When a `0x21` echoes `0C 60 <high> <low>`, the bytes
   after that header are also reported as `value: xx xx …`. They are not interpreted, and nothing is
   applied to the device record. An `ERROR_RESP` is decoded like for every other probe.
7. **Registration.** A domestic hot water device has no entity. Declaring it as a `switch:` with
   `io_device_type: 0x33` (a raw value; the kind has no YAML name) registers it so the probes can reach
   it. The switch does nothing: toggling is refused by the entity-class check, and the initial status
   request is dropped before anything is queued.

## Consequences

- The `0x20` frame is no longer "never sent by a probe". Writing stays excluded: a register write needs
  a new decision and a new builder.
- Which node answers is unknown for the water heater. An AGILIA radiator answers one address above its
  pairing address; the docs tell the user to probe the paired address first and, if every read is
  refused or unanswered, to register the next address up and probe that.
- Some registers will answer with an error or not at all; each result is worth recording.
