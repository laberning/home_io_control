# ADR 0134: The component supports ESPHome 2026.6 and newer

<!-- doxygen-label: adr0134 -->

**Status:** Accepted · **Recorded:** 2026-11

## Context

The component is an external component: it compiles against whatever ESPHome version the user runs, and
nothing said which versions that may be. The project builds and tests against one pinned image, so a change
that starts using a newer ESPHome API passes every check and breaks only on a user's older installation.
That happened twice in November 2026 ([issue #74](https://github.com/laberning/home_io_control/issues/74)):
the light write gate called `LightState::is_transformer_active()` through a const pointer, which ESPHome
declares const only from 2026.9, and the one-way light's codegen used `light.GammaTable`, also new in 2026.9.
On ESPHome 2026.8 the component did not compile at all, and the user saw a compiler error or a Python
traceback, not a version requirement.

Comparing the ESPHome releases with what the component uses gives the oldest version it can build with:

- `cv.sensitive()`, which masks every `system_key` in `esphome config` and the dashboard, exists from
  ESPHome 2026.6.0.
- The scheduler's `set_timeout()` with a `const void *` key, which the LR1121 firmware update and the
  entities' first status request use, exists from 2026.5.
- Everything else the codegen and the C++ code use exists in 2026.6.0, and the test configuration
  compiles against ESPHome 2026.6.0 and 2026.8.2.

## Decision

1. **The minimum is ESPHome 2026.6.0.** It is `MIN_ESPHOME_VERSION` in `hub_names.py`, the one place the
   number lives.
2. **The hub's schema checks it first.** `cv.require_esphome_version()` is the first validator of
   `home_io_control:`, so an older ESPHome reports the minimum instead of a later error. The component's
   modules still import on an older ESPHome for that: the one use of `cv.sensitive()`
   (`validate_sensitive_system_key`) falls back to the plain validator where it is missing.
3. **A weekly job compiles against the minimum.** `make firmware-test-minimum` compiles one test configuration
   with the minimum's image; the weekly workflow runs it next to the dev and beta channels and opens an issue
   when it fails. The copy it compiles drops the test configuration's keys that are newer than the minimum and
   change nothing the component compiles (`relation:` on a sub-device, `merge_warnings:`).
4. **The minimum moves deliberately.** A change that needs something newer either guards it, as the gamma
   table is guarded (the light gets one only where ESPHome has it), or raises the minimum in the same change,
   with a new record and a line in the getting-started page.

## Consequences

- A user on an older ESPHome gets one message naming the version to update to.
- The getting-started page names the minimum, so a user can check before flashing.
- A change that uses a newer ESPHome API fails the weekly job instead of a user's build; it still passes the
  per-change CI, which uses the pinned image.
- The minimum is checked by compiling one configuration, not the codegen tests: those use keys and fixtures
  newer than the minimum.

## Considered options

- **No stated minimum, support "recent" versions.** What happened on issue #74: nobody knows a version is
  unsupported until a build fails.
- **A rolling window, such as the last three releases.** Easy to state, but it moves without any change to the
  component, and every move would need the same check as a fixed minimum.
- **Only the pinned version.** Simplest to test, but users of the ESPHome Device Builder update ESPHome on
  their own schedule, and nothing the component uses needs the newest release.
