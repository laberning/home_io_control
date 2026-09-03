# ADR 0032: 1W enrollment follows the gesture the target's `manufacturer:` expects

**Status:** Accepted · **Recorded:** 2026-09

## Context

ADR 0031 made the 1W *control* frames (`CMD_EXECUTE`) vendor-correct via
`manufacturer:`. The *enrollment* frames (`CMD_ONEWAY_ADD_CONTROLLER` 0x30 /
`CMD_ONEWAY_REMOVE` 0x39) were still shaped entirely from Somfy: `send_enrollment()`
sent one `0x39` then one `0x30`, both to the identity's own `io_device_type`.

A real VELUX KLI 310/311/312/313 PROG press does something structurally different.
Reconstructed from two independent sources — the issue #74 capture of a real KLI 310
gesture (decoded with this project's own `broadcast_target_type()`) and
`samr037/iohc-flipper`'s `tx_runner.c` (`send_pair_with_identity`, built from real
KLI 313 "rings" captures), cross-checked against the KLI manual:

1. **`0x39` clear** goes to the all-devices broadcast `00 00 3F`, not a typed class.
2. **`0x30` add-controller is swept** across exactly three typed classes —
   `roller_shutter` (`00 00 BF`), `awning` (`00 00 FF`), `dual_shutter` (`00 03 7F`)
   — under **one shared rolling sequence**. A KLI remote never enrolls to
   `screen` / `blind` / `venetian_blind`; the receiver filters by its own class and
   only the matching frame registers the controller.
3. **A STOP then a DOWN `CMD_EXECUTE`** follow the sweep, to `00 00 3F` at the VELUX
   ACEI — the KLI manual's "press PAIR, then STOP then DOWN within 3 seconds"
   registration completion. The motor opens the 3-second window when it receives
   the `0x30`.

The `0x39` prelude itself is **not** vendor-specific — a real Somfy Smoove capture
(`somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml`) and the KLI 310
both send it, and `iohc-flipper` sends it for both vendors. An earlier plan draft to
make it opt-in was wrong and is not adopted.

Full analysis: `analysis/velux_vs_somfy_1w_frame_differences.md` §6.

## Decision

`OneWayWireProfile` (`oneway_controller.h`, from ADR 0031) gains:

- `EnrollGesture enroll_gesture` — `SOMFY` or `VELUX_KLI`.
- `std::array<DeviceType, 3> enrollment_classes` — the `0x30` sweep target list
  (`{roller_shutter, awning, dual_shutter}` for VELUX; all-`UNKNOWN` for SOMFY).

`resolve_oneway_wire_profile()` maps manufacturer `velux` → `VELUX_KLI` + that list;
`somfy` / unset / any unprofiled vendor → `SOMFY` + empty. A per-identity
`enrollment_classes:` YAML key overrides the profile list (via
`effective_enrollment_classes()`), for a user who has narrowed which class their
actuator listens on.

`send_enrollment()` dispatches on `enroll_gesture`:

- **`SOMFY`** (`send_somfy_enrollment_()`): byte-for-byte the pre-ADR-0032 behaviour
  — `0x39` then `0x30`, both to `io_device_type`, `enrollment_with_mac` honoured.
- **`VELUX_KLI`** (`send_velux_kli_enrollment_()`): `0x39` → `00 00 3F`; then
  `send_enroll_sweep_()` reserves one sequence and bursts a `0x30` to each
  non-`UNKNOWN` class under it; then a STOP and a DOWN `CMD_EXECUTE` to `00 00 3F`
  at the VELUX ACEI, one sequence each. The gesture consumes 4 sequences total
  (`0x39`, sweep, STOP, DOWN). A failed `0x39` prelude or STOP/DOWN follow-up only
  warns; the sweep is what the return value reflects.

The schema warns when a `manufacturer: velux` + `enrollment: true` identity has an
`io_device_type` of `screen` / `blind` / `venetian_blind` and no
`enrollment_classes:` override — the sweep will ignore that `io_device_type`.

### Blocking time

The VELUX gesture is 6 bursts (`0x39` + a 3-class `0x30` sweep + STOP + DOWN), each
~1 s on air with `LONG_PREAMBLE` on every copy — so it blocks `loop()` for ~6 s,
feeding the watchdog in the gaps. This is ~2.3× ESPHome's 2550 ms loop-warning
threshold (ADR 0013). **The exemption is taken knowingly**: 1W enrollment is a
user-initiated, once-per-device action, the same shape as the pairing button, whose
`pairing_discovery_wait_ms` already goes to 5000. Shrinking it (a short preamble on
burst copies 2-4, what real remotes do) would change the on-air shape of *every* 1W
transmit including the hardware-validated Somfy path — its own change, its own
hardware gate (`analysis/oneway_vendor_wire_profile_plan.md` §8).

### The 3-second window may not fit (plan §7.2 consequence 2)

The KLI manual specifies STOP then DOWN **within 3 seconds** — of what, the manual
does not say, but the motor most plausibly opens that window when it receives the
`0x30`. Three `0x30` bursts alone are ~2.9 s at the `LONG_PREAMBLE`-on-every-copy
cadence, so the STOP lands ~4 s and the DOWN ~5 s after the *first* `0x30` —
plausibly outside the window the follow-up exists to hit. This is the strongest
reason the shipped gesture may be structurally unable to complete a VELUX
enrollment, and it is not resolvable from the desk: the short-preamble follow-up
above would cut a burst to ~230 ms and largely remove the problem, and the issue
#74 "capture a real KLI PROG gesture in full" ask is the only artefact whose
inter-frame timing settles it. Shipped as-is on the judgement that getting the
*frame shapes* right is the prerequisite either way.

## Consequences

- A `manufacturer: velux` enroll button now emits the KLI gesture. Whether a VELUX
  actuator actually registers from it is **unconfirmed** — no project has shown a
  hub can 1W-enroll on VELUX at all (#74's KUX 110 never reacted, most likely
  because its own PROG window was not held open at `0x30` TX time, ADR 0026 — a
  physical gesture, not a frame-shape problem). The STOP+DOWN half in particular is
  single-sourced (`iohc-flipper`) and not confirmed against a VELUX capture.
- The Somfy enrollment path is untouched — same code, moved into a named method.
- `synthetic_enrollment_velux_kli_prog_sweep.yaml` is the golden reference for the
  VELUX gesture's wire shape; its STOP+DOWN frames may be corrected once a real
  VELUX enrollment is captured (hex is immutable, so that means delete + re-add).
- The profiled-vendor set is now written in three places (the C++ `switch`, the
  Python `_ONEWAY_WIRE_PROFILE_MANUFACTURERS`, and the schema warning) with no
  automated sync check — cross-referenced in comments, same treatment as ADR 0031.
