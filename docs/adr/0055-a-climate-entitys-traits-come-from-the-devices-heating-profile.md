# ADR 0055: A climate entity's traits come from the device's heating profile

<!-- doxygen-label: adr0055 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

The climate entity was written for one device, the electrical heater with an adjustable setpoint
(node type 52, subtype 1), and its modes, presets, temperature range and register writes were
constants of that heater. The plain electrical heater (52/0) is also a heater, but it has no setpoint,
no measured temperature and no private registers: it is commanded with a heating level in the
standard EXECUTE (off, frost protection, eco, two comfort offsets, comfort, boost, and a secured level
that the device reports and a controller cannot set). Sending it the register protocol would write
frames it does not understand, and showing it a setpoint card would offer a control that does nothing.

One entity has to show different cards for different heaters, and the gate that says "a climate entity
may bind" can no longer also mean "the private register protocol may be sent".

## Decision

1. **One profile per heater kind.** `proto_heating_profiles` holds a row per (type, subtype): the
   control path (`PRIVATE_REGISTERS` or `HEATING_LEVEL`), the HVAC modes and presets with the wire
   value each writes, the table that maps a value the device reports back onto the card, which
   registers a poll reads, and which features and companions exist. The lookup follows the type
   table's rule: an exact row, else the type's own row. The platform asks the profile and has no
   per-device-type branch; it dispatches on the control path only.

2. **Two gates.** `device_supports_climate_control()` is "a profile exists": a climate entity may
   bind. `device_uses_heating_registers()` is "the profile's path is `PRIVATE_REGISTERS`": every
   `CMD_WRITE_PRIVATE` entry point (the entity's writes and reads, the `heating_control` action, the
   poll) asks it. A plain heater therefore never receives a register frame from any entry point.

3. **Heating levels travel in the main parameter.** A level is the low byte of a main parameter
   whose high byte is `0xFC`, sent in a main-parameter-only EXECUTE through the same funnel as the
   other execute-family operations. The status request returns the current level the same way; the
   hub stores it in `IoDevice::heating.level`, and the settle poll after a level write reads it back.
   A main parameter that is not a level is logged and leaves the level unread.

4. **The card is observed.** As in ADR 0054, the entity publishes what the device reported. Mode and
   preset come from one table per profile (`observed`), so the program mode of a register heater and
   every level of a plain heater map to a mode plus an optional preset in the same way. A preset that
   no register reports (presence) stays last-commanded and is marked in the profile.

5. **A level heater is polled on the heater cadence.** It uses the ordinary 0x03 status read, a
   tracking window for the failure ladder, and the heater cadence timer (own timer id, ADR 0054).

6. **Companion entities follow the declared kind.** The window sensor and the open-window-detection
   switch exist for profiles whose features say so. ESPHome sizes its component vector before
   `to_code()` runs, so `climate.py` declares their IDs during validation from the declared
   `io_device_type` and `io_subtype`, using one small table (`HEATING_WINDOW_PROFILES`) that a sync
   test keeps equal to the profile features. A heater declared without a type gets no companions.

## Considered

- **Branch on the device type inside the entity.** Rejected: every new heater kind would add `if`s to
  the platform, and the traits, writes and read-backs would drift apart.
- **A separate `climate` platform for level heaters.** Rejected: both are `climate:` entries for the
  user, and the YAML snippet the pairing flow prints is one platform name per type.
- **Generate the companions for every climate entity.** Rejected: a plain heater would show a window
  sensor that never has a state.

## Consequences

- Heating-level control of the plain heater is experimental and unconfirmed on hardware; a capture or
  a reporter's heater is what makes it supported. The profile keys on (type, subtype) only, so a
  non-Atlantic 52/1 that takes levels would need its own row.
- The card of a plain heater shows ESPHome's setpoint control, which the entity ignores with a log
  line, because ESPHome cannot hide it.
- The companions need `io_device_type: "electrical_heater"` and `io_subtype: 1` in the YAML; the
  pairing snippet prints both.
- The open-window-detection setting is read once, at the first poll, so a change at the heater's
  panel appears after a reboot.
- Adding a heater kind is a profile row, a type-table row with a platform, and, if it has the window
  companions, a line in `HEATING_WINDOW_PROFILES`.
