# ADR 0090: A water heater's modes are chosen by what heats

<!-- doxygen-label: adr0090 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) (a domestic hot water device is
a water heater): the mode table and the rule behind it.

## Context

ESPHome's `water_heater` has a fixed list of modes (off, eco, electric, performance, high demand, heat
pump, gas) and no custom modes; Home Assistant's ESPHome integration maps the seven values one to one.
Home Assistant defines eco as energy efficient, energy saving and fast heating, heat pump as the slowest to
heat and the one that uses the least energy, and performance as high performance.

ADR 0068 mapped the Atlantic Calypso's three settings by name: automatic to `heat_pump`, manual with ECO
on to `eco`, manual with ECO off to `performance`. Two labels then say the wrong thing. Automatic adapts to
consumption and uses the electric element as needed, which is energy saving, not heat pump only. Manual
with ECO on heats with the heat pump alone, at 50 or 54 degrees, which is what Home Assistant calls heat
pump. Manual with ECO off lets the element assist and reaches 62 degrees. These behaviours come from the
heater's user manual, as discussed in
[issue #198](https://github.com/laberning/home_io_control/issues/198). Home Assistant's own EcoNet
integration maps its Energy Saving mode to eco and its Heat Pump Only mode to heat pump.

## Decision

- **The Home Assistant mode is picked by what heats and how**, following Home Assistant's definitions of
  the modes, not by the name the heater gives the setting. The same rule applies to every water heater that
  follows.
- **The Calypso table**: automatic (0) is `eco`, manual with ECO on (1) is `heat_pump`, manual with ECO off
  (2) is `performance`. The three modes stay the only ones offered.
- **Boost and away stay out of the mode list**: boost is the opt-in switch, away is the card's away mode
  (ADR 0081).
- **`off` is offered only for a device that can be turned off.** No water heater has been seen reporting an
  off state.
- **The automatic-mode target guard reads the heater's mode**, never a fixed Home Assistant mode: the heater
  holds no target of its own to set in automatic mode, whichever Home Assistant mode that maps to.
- **An opt-in `heating_source:` text sensor** shows what heats, from the heating-state reading the poll
  already stores: `heat pump`, `electric backup`, `heat pump and electric backup` or `idle`, and no state
  until the heater has reported it. It adds no radio traffic.
- The table stays one table in the platform until a second water heater kind needs its own.
- The mapping rests on the heater's manual for the heat-pump-only setting and has not been confirmed by
  watching the element; the heating-source sensor shows what runs, so a user can check it.

## Consequences

- An automation that selects `eco` now writes the heater's automatic mode, and `heat_pump` writes manual
  with ECO on. A user whose automations relied on the earlier table has to swap the two.
- The card's labels describe what the heater does, and a voice assistant's "heat pump" request means heat
  pump only.
- A mode table for another water heater follows the rule; no mode name is borrowed from the heater's own
  display.

## Considered options

- **A climate entity.** Rejected for the reasons in ADR 0068: a climate card's modes and room temperature
  do not describe a tank.
- **Custom modes named after the heater's display.** Not possible: ESPHome's water heater has no custom
  modes.
- **Folding the three settings into fewer modes.** Rejected: it hides a setting the heater offers and the
  heat-pump-only setting would not be selectable.
