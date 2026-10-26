# ADR 0052: Device types follow the IO node-type and subtype table

<!-- doxygen-label: adr0052 -->

**Status:** Accepted, superseded in part by [ADR 0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md) (the plain electrical heater has a climate profile), amended by [ADR 0059](0059-device-capabilities-come-from-the-type-table.md) (capabilities are a word on the row) · **Recorded:** 2026-10

## Context

An IO node type is a ten-bit field and the subtype six bits (`type << 6 | subtype` in two bytes).
`DeviceType` held eight bits, so two reachable types did not fit: a remote controller (1023) was
read as `0xFF`, and a test type (1008) aliased another value. A second decoder returned the raw
ten-bit value only to work around that, and the discovery log could not name a type it had cut in
half.

What the project knew about a type was spread over five `switch` statements (identifier, YAML name,
capability class, platform name, tilt and vent), each of which had to be edited for a new type. They
named only some of the node types the public specifications list, and they knew no subtypes at all. Two
consequences showed up in field reports:

- A device is told apart by subtype as well as by type. The Atlantic AGILIA heater is type 52
  subtype 1 (an electrical heater with an adjustable setpoint); the heating payloads fit that device
  only, but the climate gate was a capability class that also accepted exterior heating and heat
  pumps, which are driven with the standard EXECUTE.
- Subtype 58 means "main parameter only" on every type (public KLF 200 API specification). The fix
  for a MOTO 1000 garage opener (issue #163) hard-coded "garage opener" instead.

Six YAML names also disagreed with the public type names: one named type 11, which is a secure
configuration device and not an actuator, and five left out the position (vertical, interior,
exterior) or the construction (roller) that distinguishes the type.

## Options considered

- **Keep `DeviceType` at 8 bits and carry the full type next to it** (the previous shape). Rejected:
  every consumer had to remember which of the two to read, and the narrow one was the one used by
  default.
- **Add the missing types to each `switch`.** Rejected: five edits per type, and nothing connects
  the subtype to any of them.
- **One table keyed by (type, subtype), a 16-bit enum, and renamed YAML names with a migration
  error** (chosen).

## Decision

- `DeviceType` is `uint16_t` and has an enumerator for every node type the public specifications
  list (0-24, 27-30, 51-53, 255, 1008, 1023). Decoders return it unnarrowed. An encoder whose wire
  field is ten bits wide drops higher bits explicitly.
- One table (`proto_device_types.cpp`) has a row per (type, subtype). A row carries the lowercase
  identifier (type rows) or a description (subtype rows), the capability class, and flags: YAML
  selectable, has a platform, standalone name, experimental, and the capability word of [ADR 0059](0059-device-capabilities-come-from-the-type-table.md). A lookup falls
  back from (type, subtype) to (type, 0). The identifier, the YAML name, the capability class and
  the platform name all read from it.
- Discovery, the pairing summary and Scan Paired Devices print `<type>, <subtype>` (for example
  `electrical heater, with adjustable setpoint`), or `subtype N` when the subtype has no name. A
  type without a platform prints `not supported yet` and gets no paste-ready YAML.
- Behaviour keys on the row. Its capability word says that subtype 58 and every garage opener take the
  main parameter only ([ADR 0059](0059-device-capabilities-come-from-the-type-table.md)). `device_supports_climate_control(type, subtype)` is true exactly for
  type 52 subtype 1. Lights of subtype 1 and 2 are brightness-only (their snippet sets `dimmable:`);
  those rows are experimental until a capture confirms them.
- Six YAML names change to the public type names: `secure_configuration_device` (type 11, no
  platform and not selectable), `vertical_interior_blind`, `interior_venetian_blind`,
  `vertical_exterior_awning`, `dual_roller_shutter` and `exterior_venetian_blind`. The retired names
  fail validation with the replacement in the message. `electrical_heater` (52) and
  `sliding_window` (27) are new selectable names. The
  [node types table](../supported-devices.md#node-types-and-subtypes) lists every name.
- `io_device_type` accepts a raw value up to 1023.

## Consequences

- **Breaking configuration change.** A config that uses a retired name fails at validation with the
  name to switch to. The name that stood for type 11 has no direct successor: the user picks the
  type their device reports.
- **The heating temperature interface has no YAML name.** Only raw `0x0E` validates, and a
  `climate:` entity bound to it, to exterior heating, a heat pump or the plain electrical heater
  fails at boot. The heating payloads do not fit those devices; they
  need the standard EXECUTE, which no platform sends yet.
- **Adding a type is one table row and one enumerator.** The sync test fails when the table, the
  enum and the YAML option list disagree.
- **Unconfirmed rows are experimental.** Rows that no capture or report has confirmed yet are marked
  experimental in the table and in the docs.
