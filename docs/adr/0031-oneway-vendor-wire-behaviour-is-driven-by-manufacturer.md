# ADR 0031: 1W vendor wire behaviour is driven by `manufacturer:`, and `execute_broadcast` is a remote-shape axis

**Status:** Accepted · **Recorded:** 2026-09

## Context

The 1W transmit builders were shaped entirely from Somfy captures. Two of the
bytes they emit are not vendor-neutral:

- **The ACEI byte** (payload position 1 of a `CMD_EXECUTE` frame). Somfy 1W
  remotes carry `0x43` (priority level 2); a VELUX KLI-class remote carries
  `0x61` (level 3) — verified against one corpus frame
  (`velux_kli313_oneway_stop.yaml`), `samr037/iohc-flipper`'s README (which
  names this exact byte the "vendor byte", `0x43` Somfy / `0x61` Velux), and
  `iown-homecontrol`'s generic `ACEI_DEFAULT` of `0x61`. `ONEWAY_EXECUTE_ACEI`
  was a hardcoded `0x43`, so the hub could not mimic a VELUX remote — issue #95
  (`stop_via_oneway:` on a VELUX screen) and issue #74 (VELUX 1W enrollment)
  both fail on it.
- **The destination.** `init_1w_broadcast_frame()` always addressed the typed
  per-class broadcast (`encode_broadcast_address(io_device_type)`). Real
  handheld cover remotes of *both* vendors address the all-devices broadcast
  `00 00 3F` for open/close/stop; the typed form is what a controller *bound to
  one device class* uses (our own hub-as-1W-controller for a single Somfy
  Izymo, hardware-validated on a typed LIGHT address).

`oneway_controllers:` already carries an optional `manufacturer:` key — the
alliance-assigned vendor byte, used until now only as the `0x30` enrollment
frame's `manufacturer_id`.

## Decision

**1. Vendor wire behaviour is derived from `manufacturer:`, not a new key.**
`resolve_oneway_wire_profile(manufacturer)` (pure, in `oneway_controller.h`)
maps the byte to an `OneWayWireProfile`. `manufacturer` omitted / `0` /
`somfy` → the historical Somfy-shaped default (`ONEWAY_EXECUTE_ACEI`, `0x43`),
byte-for-byte. `velux` → `ONEWAY_EXECUTE_ACEI_VELUX` (`0x61`). Any other
explicitly-named vendor → the Somfy default with `profile_is_a_guess = true`,
which the Python schema turns into a build-time warning (not a `cv.Invalid`:
an unprofiled vendor is a legal config that still transmits, and `manufacturer`
is *required* for `enrollment: true`). A per-identity `execute_acei:` override
(rejected as `0` in the schema, the C++ "not overridden" sentinel) always wins.

**2. `execute_broadcast: typed | all` is a separate axis, not part of the
vendor profile.** The corpus shows no vendor difference on the EXECUTE
destination — a Somfy handheld and a VELUX handheld both use `00 00 3F`, and a
class-bound identity of either vendor uses the typed form. Coupling it to
`manufacturer` would invent a distinction the wire does not have. Default is
`typed` (byte-for-byte today's behaviour; a flip would silently retarget the
one hardware-validated class-bound configuration with no failing test);
`all` is the documented one-liner for "mimic a handheld remote".

## Consequences

- **Zero back-compat impact.** Every existing `oneway_controllers:` entry
  (no `manufacturer:`, or `manufacturer: somfy`) emits exactly the bytes it
  did before. Pinned by `OneWaySendCommandTest.IdentityWithoutManufacturerStillEmitsTheSomfyAcei`.
- **`ONEWAY_EXECUTE_ACEI` moved to `proto_constants.h`.** The resolver is
  controller-layer; the protocol layer (`proto_commands.cpp`) must not depend
  on it (AGENTS.md layering rule 1). `proto_constants.h` already holds the
  `ACEI_*` and `MANUFACTURER_*` constants both sides need.
- **The profiled-vendor set is written twice** — the C++ `switch` and the
  Python `_ONEWAY_WIRE_PROFILE_MANUFACTURERS` — with no automated sync check
  (`check-yaml-emitters.py` compares key *names*, not table contents).
  Cross-referenced in both comments; promote to a generated table if it grows
  past a handful.
- **`execute_broadcast: all` makes the wire destination decode to
  `DeviceType::UNKNOWN`.** The `OneWayCommandReport` therefore takes
  `target_type` from the *identity's* class, not the built frame's, so the
  "Last 1W Command" sensor and the TX log still read `STOP -> awning`, not
  `STOP -> unknown`. (The boot log was never affected — it prints `class 0x%02X`
  straight from `io_device_type`.) Pinned by
  `OneWaySendCommandTest.ExecuteBroadcastAllDoesNotDegradeTheReportToUnknown`.
- **VELUX enrollment is not addressed here.** A VELUX KLI PROG gesture sweeps
  `0x30` across three classes and adds a STOP+DOWN follow-up; that is Phase 2
  of `analysis/oneway_vendor_wire_profile_plan.md`, gated on a real VELUX
  device and on a full KLI-PROG capture. Phase 1 (this ADR) makes the *control*
  bytes right; it cannot make a device that has never registered the hub obey
  it.
- **The VELUX ACEI rests on n=1.** `0x61` is also `iown-homecontrol`'s generic
  default, equally consistent with "VELUX uses it" and "non-Somfy remotes use
  it". `execute_acei:` is the escape hatch; the constant's doc comment records
  the confidence.
- Nothing else on the wire changes: 6-byte payload form, originator, sequence,
  MAC span, repeat cadence, ctrl1 — all vendor-independent and untouched.
