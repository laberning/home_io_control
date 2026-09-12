# ADR 0036: Hub-level entities come from `home_io_control:` flags, never a platform entry
<!-- doxygen-label: adr0036 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

Every hub-level control entity — the two arming switches (`accept_foreign_pairing`,
`recover_oneway_key`), Scan Paired Devices, the `tuning: {ui_controls: true}` numbers/selects, each
1W identity's command buttons — is created dynamically from a flag inside the `home_io_control:`
block. None of them needs a device to bind to (see `HubBoundEntity`), so none of them takes a
separate platform entry.

Discover & Pair was the one exception: it shipped early as its own `button:` platform
(`button.py`), before the flag-driven shape existed, and was never migrated. That inconsistency —
one hub-level button needing a whole extra top-level YAML key that its siblings don't — is what
this ADR fixes.

## Options considered

**Default the new `home_io_control.discover_and_pair_button` flag to `true`,** so a bare hub block
reproduces "the button just works" for new users. Rejected: it doesn't reproduce anything, since a
bare hub block creates no pairing button today either — the legacy `button:` entry is the only
thing that ever did. Worse, it actively breaks every user who *does* have that entry: ESPHome
refuses two `button` entities with the same object id, the hub's dynamic creation runs inside
`to_code()` (after the legacy entry has already claimed the name during earlier schema
validation), and that collision surfaces as an uncaught `cv.Invalid` traceback, not a clean
validation message. The flag defaults to `false`, exactly like its three siblings.

**Auto-suppress the duplicate via a `FINAL_VALIDATE_SCHEMA` cross-check** (detect the legacy entry
from the hub's own final-validate pass and silently skip the dynamic button). Rejected: the
companion ID for a dynamically-created entity has to be declared during `CONFIG_SCHEMA` evaluation,
long before `FINAL_VALIDATE_SCHEMA` runs (see ADR 0009 — an ID declared any later is silently
dropped at runtime). Un-declaring one after the fact, once the component count has already been
sized from it, is unverified territory for a purely cosmetic problem. With the flag defaulting to
`false`, the collision this would have suppressed cannot happen in the first place.

## Decision

Add `discover_and_pair_button` (default `false`) to the hub schema, wired exactly like
`scan_paired_devices_button`: an `_inject_hub_entity_id()`-based post-validator declares the
button's and its companion "Last Pairing Result" sensor's IDs during schema validation, and a
`_create_discover_and_pair_button()` helper builds both from `to_code()` when the flag is set.

The legacy `button:` platform (`button.py`) is not deleted outright. It keeps working exactly as
before, with two additions:

- A validator, first in its `CONFIG_SCHEMA` chain, that unconditionally logs a deprecation warning
  naming the replacement flag.
- Its own `FINAL_VALIDATE_SCHEMA` (platform modules get one independent of the hub's own, which is
  already used for address-collision detection) that raises a precise `cv.Invalid` if a config sets
  *both* the legacy entry and the new flag — turning the one plausible half-migration mistake into
  a sentence naming the fix, instead of ESPHome's generic duplicate-entity traceback.

Removal is deferred to a later release: turn `CONFIG_SCHEMA` into `cv.invalid(...)` (ESPHome's own
idiom for a retired platform), then delete the file entirely once nobody's build depends on it.

## Consequences

- Every hub-level entity is now created the same way, from the same kind of flag — the whole point
  of this change.
- A user migrating off the legacy platform loses the ability to give the button a custom `name:`,
  `device_id:`, or `disabled_by_default:` — none of the flag-created hub entities support that, and
  nobody in this project's own docs or examples used it.
- The migration is two lines, not a rewrite: delete the `button:` entry, add
  `discover_and_pair_button: true`. Doing neither is safe (the entry keeps working, just with a
  warning); doing only the second without the first is caught explicitly rather than left to
  ESPHome's generic error.
- Flipping the flag's default to `true` becomes safe once the legacy platform is a hard error
  (Phase 2) — a leftover entry can no longer collide with it — but that is a separate, deferred
  product decision, not part of this change.
- The both-configured guard rejects the combination unconditionally, regardless of the legacy
  entry's own `name:`/`device_id:` — a renamed or re-homed entry could in principle coexist with
  the flag without an actual name collision, but checking that precisely would mean re-deriving
  ESPHome's own object-id logic. Rejecting outright keeps the guard simple and the migration
  unambiguous (one pairing trigger, not "two unless you were careful"). It also only covers *this*
  component's platform: a `button:` entry from an unrelated platform sharing the same object id
  (e.g. `platform: template` also named "Discover & Pair") is not caught and still hits ESPHome's
  generic traceback — an accepted, narrow residual gap rather than something worth generalizing
  this guard for.
