# ADR 0126: Water heaters and the micromodule switch are not experimental

<!-- doxygen-label: adr0126 -->

**Status:** Accepted, amends [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) · **Recorded:** 2026-11

## Context

A type-table row carries the experimental flag while no capture or report confirms it on a real
device ([ADR 0059](0059-device-capabilities-come-from-the-type-table.md)). Discovery and pairing then
log the kind as experimental, the build warns when a config declares it, and the node-type table in
the docs marks it.

ADR 0068 flagged row 51/0, domestic hot water, as the one experimental type row. One heater of one
brand had been read at the time and its writes were unconfirmed. The build-time warning was also
what told the owner of a water heater of another brand that its registers might differ. The two
model rows below 51/0 took the same flag.

Since then three water heaters are confirmed on hardware:

- an Atlantic Calypso Connecté: readings, target, mode, boost and away (issue #198, corpus
  `atlantic_calypso_*`);
- an Atlantic Explorer V4, declared with `io_model: explorer_v4`: readings, target, mode, boost and
  away (pull request #246);
- a Sauter Guelma connecté, declared with `io_model: electric_water_heater`: register reads and
  writes (issue #247, corpus `sauter_guelma_*`).

Row 15/2, the micromodule on/off switch, was flagged because nothing confirmed its name or its
behaviour. A Somfy Izymo on/off receiver now pairs as 15/2, switches on and off and follows a linked
remote (issue #57).

## Decision

Rows 51/0, its model rows `explorer_v4` and `electric_water_heater`, and row 15/2 carry no
experimental flag. No type row (subtype 0) carries the flag any more; it marks subtype rows only.

## Consequences

- A config that declares `domestic_hot_water` or an `on_off_switch` of subtype 2 builds without the
  experimental warning, and discovery, pairing and the docs name these kinds without the marker.
- A water heater of another brand that reports 51/0 gets the same reads and no build-time warning.
  If its registers differ, its readings stay empty or wrong, and the brand line in the log is what
  tells the user. The water heater pages name the three confirmed heaters, so an owner of another
  one can see that theirs is not among them.
- Single functions of a confirmed kind can still be unconfirmed. They are marked on the page of the
  device or platform, not by the row's flag.
- The switch rests on one field report and no corpus capture. A capture of a switch would confirm
  startup state restoration, which the report does not cover.
