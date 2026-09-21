# ADR 0041: An unset 1W `low_power:` resolves from the manufacturer profile

<!-- doxygen-label: adr0041 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

[ADR 0038](0038-oneway-bursts-follow-the-identity-power-class.md) made a 1W identity's burst shape
configurable through a tri-state `low_power:` key, and left *unset* meaning `LEGACY_LONG` — the
1024-byte preamble on all four copies — so that no existing installation's on-air bytes moved.

That default cannot work on a VELUX receiver. An awake VELUX receiver does not accept a frame
behind the long preamble; it accepts the same frame behind a short one. The effect was isolated on
2W, by a bisection that varied the preamble and the `CTRL1_LOW_POWER` flag
independently, and replicated on 1W: a mains-fed VELUX roller shutter did not move for
a burst sent `1024/1024/1024/1024`, and moved for both shapes that put any copy at 32 bytes, with
`CTRL1` held at `0x00` throughout and nothing else changed.

**1W carries no acknowledgement**, so the hub cannot tell a burst that was obeyed from one that was
ignored. The default therefore turns a one-line omission into a silent failure that presents as
"pairing does not work", with nothing in the logs pointing at the radio.

ADR 0038 named the evidence that would settle whether the default should change — "a commands-only
A/B on an already-enrolled identity" — and issue #74 ran exactly that.

The same reasoning does not extend to Somfy. A mains-fed Somfy awning has been measured taking the
long preamble reliably while missing a substantial fraction of short start frames, so a blanket
change would risk a regression on the one vendor path this project has validated most.

## Options considered

- **Default everything to `ALWAYS_ALIVE`** (full ADR 0029 parity). Rejected for now: it moves
  Somfy's on-air bytes with no Somfy evidence, and the measurement above suggests the move could
  hurt. This remains the intended end state once a Somfy regression run exists.
- **Default everything to `LOW_POWER`** (one long wake copy, three short repeats). Tempting, since
  it serves mains and duty-cycled receivers alike and matches a real KLI remote's burst. Rejected:
  it is not byte-identical to legacy for Somfy either — copy 1 gains `CTRL1_LOW_POWER`. The 1W MAC
  spans cmd+data only, so the MAC stays valid, but a receiver filtering on `CTRL1` would break, and
  nothing rules that out.
- **Resolve the unset default from the manufacturer profile** (chosen). The evidence is
  manufacturer-shaped, so the default is too.
- **Keep the legacy default and document the workaround.** Rejected: the documentation already told
  VELUX users to set the key, and they still hit the failure, because nothing in the symptom points
  at a radio setting.

## Decision

`OneWayWireProfile` (`oneway_controller.h`) gains `default_power_class`, the shape used by an
identity that does not set `low_power:` at all:

| `manufacturer:` | `default_power_class` | Why |
|---|---|---|
| `velux` | `ALWAYS_ALIVE` | Issues #87 and #74: an awake VELUX receiver rejects the long preamble. |
| `somfy`, unset | `LEGACY_LONG` | Byte-identical to before; no Somfy regression run exists. |
| anything else (`profile_is_a_guess`) | `LEGACY_LONG` | A profile nobody has measured never silently acquires a shape nobody tested for it. |

This follows the pattern ADR 0031 already established for `execute_acei` and ADR 0032 for
`enrollment_classes`: a manufacturer profile supplies the default, an explicit YAML key overrides
it, and one `effective_*()` helper is the only place the two are combined.

- **The identity stores intent, not a shape.** `OneWayControllerIdentity::power_class_override` is
  a `std::optional<OneWayPowerClass>`, empty when the key is absent. `OneWayPowerClass` keeps
  exactly three enumerators, each a real on-air shape, so `oneway_burst_copy_shape()` still
  switches exhaustively with no unreachable case. A fourth "unset" enumerator was avoided precisely
  because it would make that function accept a value it cannot render.
- **`effective_power_class(identity)` is the single resolver**, and the only thing the transmitter
  and the boot log ask. `has_power_class_override()` reports which side the answer came from.
- **Resolution stays in C++.** Codegen emits the three configured states as three distinct values
  (`{}`, `ALWAYS_ALIVE`, `LOW_POWER`) and never consults the manufacturer, so the profile lookup
  lives in one language rather than being duplicated in `__init__.py` where it would drift from the
  ACEI and enrollment-class defaults beside it.
- **The boot log prints the resolved class and its origin** — `power always-alive (from profile)` —
  since the class alone would not tell a VELUX user whether they had chosen it or inherited it.

## Consequences

- **A VELUX 1W identity that omits `low_power:` changes shape**, from `1024/1024/1024/1024` to
  `32/32/32/32`. For those users this is the difference between a hub that does nothing and one
  that works; there is no configuration for which the old default was better on that hardware.
- **Somfy and unrecognised manufacturers are byte-identical to ADR 0038.** Nothing about their
  transmissions changes.
- **An explicit `low_power:` still wins everywhere**, so a user who pinned a shape keeps it, and a
  VELUX user with an unusual receiver can still force `LEGACY_LONG` back.
- **Unset is now manufacturer-dependent, which is one more thing `low_power:` means.** The key was
  already tri-state and already differs from the 2W key of the same name; the boot log's
  `(from profile)` marker exists so the resolved answer is never a guess.
- **ADR 0029 parity is still not reached.** `LEGACY_LONG` remains the default for Somfy, still
  violating the "flag and preamble never disagree" invariant, until a Somfy regression run says
  otherwise. This ADR narrows that debt to one vendor rather than clearing it.

See also [ADR 0038](0038-oneway-bursts-follow-the-identity-power-class.md) (the shapes and the
tri-state key), [ADR 0029](0029-start-preamble-is-a-property-of-the-target.md) (the 2W precedent),
and [ADR 0031](0031-oneway-vendor-wire-behaviour-is-driven-by-manufacturer.md) (the
manufacturer-profile pattern this follows).
