# ADR 0091: Every climate preset is a command, and the heating level is a sensor

<!-- doxygen-label: adr0091 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md) (a climate
entity's traits come from the device's heating profile): which presets a profile may list, and where the
level a heater applies is shown.

## Context

A climate card has one preset at a time. Three things compete for it on an electrical heater: the variant
of a mode (an Atlantic AGILIA IO has two automatic modes, PROG and AUTO, and Home Assistant has one,
`auto`), a timed run on top of the mode (boost and drying on a towel dryer) and the level the heater
applies right now (comfort or eco while its weekly programme runs).

Home Assistant allows only its built-in HVAC modes and says: "For device options which modify how one or
more of the modes behave, add a preset"
([climate entity](https://developers.home-assistant.io/docs/core/entity/climate/)). A preset is therefore
something a user selects.

Three profiles listed presets that only showed state. The towel dryer listed `comfort` and `eco` for the
period of its programme, and the plain heater and the pilot-wire interface listed `Secured`, a level a
heater reports and never takes. Selecting one sent nothing and logged a warning, which a user cannot know
from the card. A profile row carried a flag for it.

The level itself is on the wire in one form for every electrical heater seen. The status reply's main
parameter is a heating level: the plain heater is commanded and read with it (ADR 0055, point 3), and an
Atlantic Adelis towel dryer, a register heater, answers `0xFC05` in a comfort period, `0xFC02` in an eco
period and `0xFC07` in a boost (corpus `atlantic_adelis_probe_status_mp_fp_programme`,
`atlantic_adelis_probe_status_mp_fp`,
[issue #202](https://github.com/laberning/home_io_control/issues/202)). The towel dryer's period was
instead worked out by comparing three registers: the temperature in force, the comfort setpoint and the
eco setback.

Home Assistant's Overkiz integration shows the level as the preset of an Atlantic radiator and then cannot
show which automatic mode runs; for the towel dryer it uses the preset for the programme and handles boost
and drying as special cases.

## Decision

- **Every preset a profile lists sends a command.** A profile row has no way to mark a preset as shown
  only, so the rule holds by construction. `comfort` and `eco` leave the towel dryer's presets; `Secured`
  leaves the plain heater's and the pilot-wire interface's. A preset that is a command and also reads back
  (a level on a plain heater, boost on a towel dryer) stays.
- **A preset tells two vendor modes apart only where they share one Home Assistant mode.** `Program` and
  `Auto` stay on the heater with an adjustable setpoint. A heater with one automatic mode shows `auto` and
  needs no preset for it.
- **The level a heater applies is an opt-in `heating_level:` text sensor** on an `electrical_heater`
  climate entry: `off`, `frost protection`, `eco`, `comfort -2`, `comfort -1`, `comfort`, `boost` or
  `secured`, and no state until the heater has reported a level. It follows the precedent of the water
  heater's `heating_source:` sensor
  ([ADR 0090](0090-a-water-heaters-modes-are-chosen-by-what-heats.md)).
- **The status read is the one source of the level, on every heater kind.** A register heater with a
  programme gets one status read at the end of every poll, in every mode, whether or not the sensor is
  configured. What a heater is polled for depends on its profile alone, never on which entities the YAML
  declares, so the device record is the same for every configuration and each poll has one tested path.
- **The comparison of temperature in force, comfort setpoint and setback is removed**, with the setback
  read. The card's target in the programme still shows the temperature in force.

## Consequences

- An automation that tests a towel dryer's preset for `comfort` or `eco` stops matching; it reads the
  `heating_level` sensor instead. The display was experimental.
- A register heater with a programme costs one more authenticated exchange per poll.
- The sensor stays without a state on a heater that reports no level, such as the Thermor pilot-wire
  interface ([issue #228](https://github.com/laberning/home_io_control/issues/228)).
- What the status read answers during a temporary override and during a drying run has not been captured;
  the sensor shows whatever level the heater reports then.
- A new heater kind lists the presets it takes and gets the sensor without further code.

## Considered options

- **The level as the preset, as Home Assistant's Overkiz integration does.** Rejected: the card could no
  longer tell PROG from AUTO on a radiator, and boost, drying and the level would compete for the slot on
  a towel dryer.
- **Keep shown-only presets and document them.** Rejected: the card offers a choice that is ignored.
- **Keep the register comparison as the source.** Rejected: it names two levels only, needs the setback,
  and a heater with two automatic modes would need a rule per mode.
- **Read the status only when a `heating_level:` sensor is configured.** Rejected: polling would depend on
  the YAML's companions, with two poll paths to test and a device record that differs by configuration, to
  save one exchange per poll.
