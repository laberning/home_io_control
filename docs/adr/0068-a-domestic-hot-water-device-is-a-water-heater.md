# ADR 0068: A domestic hot water device is a water heater

<!-- doxygen-label: adr0068 -->

**Status:** Accepted, amended by [ADR 0081](0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md) · **Recorded:** 2026-11

Builds on [ADR 0059](0059-device-capabilities-come-from-the-type-table.md) (capabilities come from the
type table) and [ADR 0067](0067-a-model-below-the-subtype-for-products-that-share-a-wire-type.md)
(the model argument every lookup takes).

## Context

An Atlantic Calypso Connecté heat-pump water heater pairs with Discover & Pair and reports node type 51
(`domestic_hot_water`), subtype 0
([issue #198](https://github.com/laberning/home_io_control/issues/198)). Its owner read every register
of the heater in several display states, and two corpus captures hold the answers:
`atlantic_calypso_exchange_register_read` and `atlantic_calypso_probe_status_mp_fp`.

What they show:

- The heater answers the register reads a radiator answers, `0C 60 <family> <index>` in a
  `CMD_WRITE_PRIVATE` (0x20), with a `0x21` that echoes the header and then the value. Its registers sit
  in **family 0x00**, and their multi-byte values are **big-endian**. The radiator's family 0x01
  ([ADR 0054](0054-heaters-are-read-back-over-the-private-register-protocol.md)) is little-endian.
- Register `0x0017` is the water temperature (signed tenths of a degree), `0x0006` the mode (0 automatic,
  1 manual with ECO on, 2 manual with ECO off, following the display in every run), `0x0021` what is
  heating (heat pump, electric element), `0x0000` boost, `0x0004` the energy since installation in Wh
  (the display's "since start" total to the kWh), and `0x0007` one letter naming the brand.
- The target temperature is in no register. It is the **main parameter of a status reply**, on the
  ordinary relative scale 0..`0xC800` over 20–62 °C: 54 °C reads `A1E8`, 62 °C reads `C800`, and AUTO
  reads `BE7A`, 60.0 °C. The heater answers the status read with function `0x01` (the `status_mp_fp`
  probe's shape); the plain cover status read has not been sent to it.
- No reading showed an off state, and the holiday register stayed 0 with a holiday programmed for a
  later date.

## Options considered

- **A climate profile** ([ADR 0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)).
  Rejected: a climate card has HVAC modes (off / heat / auto) and presets, and its current temperature
  is a room's. A water heater's modes are its own (heat pump, eco, performance), and Home Assistant has
  an entity for exactly this device.
- **A `water_heater` entity** (chosen). ESPHome 2026.7 ships one with the modes off, eco, electric,
  performance, high demand, heat pump and gas, and feature flags for current temperature, target
  temperature, operation mode, away and on/off.
- **A flag on the radiator codec for the byte order.** Rejected: the two families share only the frame
  shape. A separate codec keeps each family's registers and decoders in one place.

## Decision

- **The entity is ESPHome's `water_heater`**, platform `home_io_control`. Its traits are current
  temperature, target temperature (20–62 °C, step 0.5 °C) and operation mode.
- **Modes map one to one**: automatic (0) ↔ `HEAT_PUMP`, manual with ECO on (1) ↔ `ECO`, manual with ECO
  off (2) ↔ `PERFORMANCE`. `OFF` is not offered, because no off state has been seen on the wire. A mode
  value outside 0–2 leaves the shown mode as it was and is logged at DEBUG. The mapping rests on one
  heater.
- **The target is the status main parameter.** The poll reads it with the status read the heater has
  answered (function `0x01`, FP1–FP7 requested). An alias or a value beyond `0xC800` shows the target as
  unknown. The reply's other bytes never reach the cover state: a water heater has no position, no
  movement and no battery.
- **A codec of its own**, `proto_dhw.h`: family-0x00 requests, big-endian typed decoders, the mode write
  and the target scale, which reuses the temperature main-parameter encoding with 20 and 62 °C as its
  ends.
- **One capability bit and one class.** The table row 51/0 carries the `WATER_HEATER` feature bit
  (bit 10) and the `WATER_HEATER` capability class, and every decision reads the bit, including whether
  the register probes may address the device. The hub keeps the bit only on a device bound to a
  `water_heater:` entity: a type-51 device declared on another platform (a `switch:` declared only to
  reach the register probes) keeps the behaviour it had before, with no poll and no writes.
- **Row 51/0 is experimental**, the one experimental type row. Its flag names that row only: a subtype
  without a row of its own takes the type row's capabilities but not the flag, in C++ and in the Python
  mirror. A config that declares the kind gets the build-time warning, which reaches a user of any other
  water heater.
- **The writes ship, marked experimental.** The target is a main-parameter-only EXECUTE; the mode is a
  write of register `0x0006` (`0C 61 00 06 <mode>`, only the values 0–2), read back at once. After either
  write the hub polls the heater again 45 seconds later. The owner has confirmed that the display
  follows both (issue #198).
- **Companions are opt-in**: energy (kWh, `total_increasing`, from register `0x0004`), heat pump
  running and electric backup running. They show what the poll already reads. Boost is an opt-in
  switch over register `0x0000` (see Consequences).
- **The brand is logged, not enforced.** The hub reads register `0x0007` until it has an answer and logs
  the letter once at INFO. A heater of another brand is not refused at runtime.

## Consequences

- A Calypso shows in Home Assistant with its water temperature, target and mode, and its energy fits the
  energy dashboard.
- Each poll costs one status read and five register reads (the brand once more until read). The heater
  answers in about 10 ms, so the default five-minute cadence stays cheap; the interval floor is one
  minute, as for a heater.
- Boost is offered as an experimental switch that writes register `0x0000`, whose wire state has been
  seen (`1` during a one-day boost, `0` before and after it). Away waits until the holiday register
  reads non-zero while a holiday runs; off waits until a heater reports it.
- A water heater of another brand that reports 51/0 gets the same reads. If its registers differ, the
  readings stay empty or wrong; the build-time warning and the brand line in the log are what tell the
  user.
