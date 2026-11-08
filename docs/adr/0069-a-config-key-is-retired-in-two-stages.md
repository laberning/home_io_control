# ADR 0069: A config key is retired in two stages

<!-- doxygen-label: adr0069 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0065](0065-a-covers-travel-speed-is-a-three-way-choice.md) (decision 2: `silent:` stays
as the two-way form). Follows [ADR 0036](0036-hub-entities-come-from-hub-block-flags-never-a-platform-entry.md),
which retired the `button:` platform the same way.

## Context

A cover sets its travel speed with one of two keys: `silent:` (a boolean with a Silent Operation
switch) or `travel_speed:` (`default` / `silent` / `fast` with a Travel Speed select). Both set the
same field, so the docs, the schema and the C++ carry two forms of one setting, and the boolean
cannot express `default`.

This project has no releases: users build from `main` through `external_components`, so a removed
key breaks a config on the next build with no version step in between. Removing a key also changes
Home Assistant entities: here the Silent Operation switch would become a Travel Speed select, and an
automation that uses the switch stops working.

## Considered options

- **Keep both keys.** Rejected: two forms of one setting, for good.
- **Rewrite `silent:` into `travel_speed:` during validation.** Rejected: the switch would turn into
  a select without the user seeing why, and `silent: false` maps to `fast`, which a reader would not
  guess.
- **Remove `silent:` at once.** Rejected: it breaks working configs on the next build.
- **Two stages.** Chosen.

## Decision

1. **Warn.** The key keeps working unchanged. A post-validator built by
   `platform_common.deprecated_key()` logs one warning per entity: the key will become a config
   error, what to write instead, and what changes in Home Assistant.
2. **Retire.** After about two months on `main`, the warning becomes a `cv.Invalid` with the same
   replacement text, and the code behind the key is deleted. A retired platform uses ESPHome's own
   `CONFIG_SCHEMA = cv.invalid(...)`, as `button.py` does; a renamed value keeps a lookup table that
   names the new value, as the renamed device types do.

`silent:` enters stage 1 now. Its warning gives the exact value: `silent: true` is
`travel_speed: silent`, `silent: false` is `travel_speed: fast`.

## Consequences

- No config changes behaviour now; a cover with `silent:` logs one warning per build.
- At stage 2, `IOHomeCoverSilentSwitch`, `CONF_SILENT` and the exclusive-key pairing in `cover.py`
  go, and the `silent:` fixtures become `travel_speed:` fixtures.
- The test configs keep a `silent:` cover until stage 2, so the deprecated path stays compiled in CI.
