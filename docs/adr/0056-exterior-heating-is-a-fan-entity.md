# ADR 0056: Exterior heating is a fan entity

<!-- doxygen-label: adr0056 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

Exterior heating (node type 21: terrace and patio heaters) is a heater that has no setpoint and no
measured temperature. The VELUX KLF 200 API specification (Table 277 and the actuator type table,
type 21) defines its main parameter as a heat level: `0x0000` is 100 % heat and `0xC800` is 0 %, the
position scale a cover or a dimmable light uses with the direction inverted. Subtype 58 takes the main
parameter only and is on/off. The type table classified it as a climate device without a platform, so
no entity could drive it, and widening the climate gate to it would have sent the private register
protocol to a device that takes a position.

Home Assistant offers four entity shapes that could carry "on, with a 0-100 % level": light, switch,
fan and climate.

## Decision

1. **A fan entity.** Exterior heating is a `fan:` with a 100-step speed that is the heat level;
   subtype 58 advertises no speed. The type table gives the type its own capability class,
   `HEATER_LEVEL`, with the platform name `fan`, so the climate gate (a heating profile exists,
   ADR 0055) cannot reach it and the fan platform rejects every other family.

2. **The dimmable light's mapping.** Heat `p` % is the IO position `100 - p`, computed with the same
   `round_percent()` helper the light uses, and sent as a main-parameter-only execute for every
   exterior heater (21/0 and 21/58 alike). The first functional parameter of exterior heating is not a
   cover's, a guessed block is what issue #163 showed to cost a refusal (`PARAMETER_UNKNOWN`), and an
   execute without functional parameters is valid on any device. The hub gets
   `set_fan_position()` / `queue_set_fan_position()` with their own queue operation and class guard.

3. **Status read with the ordinary status request.** The reply's current position is decoded by the
   light's decoder into the existing position field; the fan shows `100 - position`.

4. **No motion.** An exterior heater never moves, so a status reply is always treated as at rest
   (`device_has_no_motion()`, shared with the heating-level climate heaters). A command leaves exactly
   one follow-up poll, and a "moving" flags byte cannot start a series.

5. **No restored state.** The fan never restores a saved state at boot: a restored command would be
   sent to the heater, and the first status poll is the truth.

## Considered

- **A light.** Rejected: Home Assistant shows a brightness slider and light services for a heater.
- **A switch.** Rejected: it has no level.
- **A climate entity.** Rejected: the device has no setpoint, and a climate card would show a
  temperature control that does nothing (the cost the plain electrical heater already pays).
- **A number entity beside a switch.** Rejected: two entities for one device, and the level and the
  state could disagree.

## Consequences

- Exterior heating is experimental: no heater has been exercised on hardware. The mapping and the
  status read rest on the public specification and stay unconfirmed until a capture or a reporter's
  heater confirms them.
- A change made at the heater itself shows after the next command or reboot: there is no periodic poll
  at rest, as for a cover.
- Turning on without a level uses the last level the entity saw, which is lost at reboot; the first
  turn-on after a boot is full heat.
- The `fan` platform needs a host stub of ESPHome's fan component, checked by `make stub-sync`.
