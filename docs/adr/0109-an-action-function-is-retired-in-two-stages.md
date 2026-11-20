# ADR 0109: An action function is retired in two stages

<!-- doxygen-label: adr0109 -->

**Status:** Accepted, amends [ADR 0069](0069-a-config-key-is-retired-in-two-stages.md) · **Recorded:** 2026-11

## Context

[ADR 0069](0069-a-config-key-is-retired-in-two-stages.md) retires a config key: a warning, then an error
about two months later. It covers keys, which are checked when the firmware is built. The function names of
the `heating_control` action are arguments of a Home Assistant action, checked when the action runs, so a
build-time warning cannot reach the people who call them. Their names also came from an older command
vocabulary (`set_temperature`, `set_mode`, `power_on`, `midnight_sync`) and no longer matched the names the
code, the logs and the entities use for the same values.

This project has no releases: users build from `main`, so a function that stops working breaks an
automation on the next build with no version step in between.

## Decision

1. **A function is named after its register.** A register's name is its `DeviceRegister` enumerator in
   lower case, and it is the register's label in logs. The action offers a function as `set_` + the name
   for a writable register and `read_` + the name for a read-only one.
2. **Warn.** A retired function name keeps working with the same frames. Every call that uses one logs a
   warning and ends the result event's `message` with the same sentence: the name is deprecated, when it
   stops working and the new name. Automation users watch the event, not the device log.
3. **Retire.** After about two months on `main` the old name is an unknown function. The error names the
   replacement, from a lookup kept for that purpose, as a renamed value does in ADR 0069.

The `heating_control` rename is the first case: `power_on`, `set_temperature`, `set_mode`,
`set_derogation`, `midnight_sync` and `window_open` become `read_paired_devices`,
`set_comfort_setpoint`, `set_operating_mode`, `set_derogated_setpoint`, `read_setpoint_block` and
`read_window_open`. They stop working after January 2027.

## Consequences

- No automation breaks now; each call with an old name logs a warning and names the new one in its result.
- The retired names live in one table beside the register table. At stage 2 the table becomes the lookup
  that names the replacement in the error.
- A register the action offers later gets its function name from its row, with no new name to choose.

## Considered options

- **Rename without a transition.** Rejected: it breaks working automations on the next build.
- **Keep the old names for good.** Rejected: two names for each function, and names that say less than
  the register's own.
- **Name the functions after the readings** (`setpoint`, `mode`, `occupancy`). Rejected: readings exist
  for only some of the registers, and `setpoint` and `mode` are ambiguous on a hub that also drives
  water heaters.
