# ADR 0032: 1W enrollment follows the gesture the target's `manufacturer:` expects
<!-- doxygen-label: adr0032 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

ADR 0031 made the 1W *control* frames (`CMD_EXECUTE`) vendor-correct via
`manufacturer:`. The *enrollment* frames (`CMD_ONEWAY_ADD_CONTROLLER` 0x30 /
`CMD_ONEWAY_REMOVE` 0x39) were still shaped entirely from Somfy: `send_enrollment()`
sent one `0x39` then one `0x30`, both to the identity's own `io_device_type`.

A real VELUX KLI 310/311/312/313 registration gesture does something structurally
different. Reconstructed from two independent sources — the issue #74 capture of a
real KLI 310 gesture (decoded with this project's own `broadcast_target_type()`) and
`samr037/iohc-flipper`'s `tx_runner.c` (`send_pair_with_identity`, built from real
KLI 313 "rings" captures), cross-checked against the KLI manual:

1. **`0x39` clear** goes to the all-devices broadcast `00 00 3F`, not a typed class.
2. **`0x30` add-controller is swept** across several typed classes under **one shared
   rolling sequence**. The receiver presumably filters by its own class, so only the
   matching frame registers the controller. The class set depends on the remote. A KLI 310/313
   (exterior shading) sweeps `roller_shutter` (`00 00 BF`), `awning` (`00 00 FF`) and
   `dual_shutter` (`00 03 7F`). A KLI 312 driving interior blinds names `blind`
   (`00 02 BF`) and `venetian_blind` (`00 00 7F`) in its `0x2E` instead, and a sweep
   over those registers.
3. **A STOP then a DOWN `CMD_EXECUTE`** follow the sweep, to `00 00 3F` at the VELUX
   ACEI — the KLI manual's STOP-then-DOWN registration completion.

The receiver side is **the Gear button ("open for registration"), pressed for about
1 second on an already-registered VELUX control**. In the KLI manual's add-a-control
procedure, Gear on the existing control is followed by the **Pair** button ("register")
on the new control, which is the role this gesture plays: the frames above are what a
KLI's Pair press sends. (Gear then Pair on the *same* switch deletes all its products
instead.) The existing control's Gear press sends a `0x2E` burst
to a set of device classes, and the product runs a "ready" sequence. On a shutter this
takes around half a minute: it travels to about 10% closed, jogs several times, then
returns to its starting position. In both confirmed enrollments (below), sweeping
exactly those classes after the ready sequence registered the hub. How long the
product stays open for registration after the sequence is not measured. The manual's
"STOP then DOWN within 3 seconds" wording belongs to the from-scratch registration
flow that starts with P on the product itself, not to this add-a-control flow.

The `0x39` prelude itself is **not** vendor-specific — a real Somfy Smoove capture
(`somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml`) and the KLI 310
both send it, and `iohc-flipper` sends it for both vendors, so it is not opt-in.

## Decision

`OneWayWireProfile` (`oneway_controller.h`, from ADR 0031) gains:

- `EnrollGesture enroll_gesture` — `SOMFY` or `VELUX_KLI`.
- `std::array<DeviceType, 3> enrollment_classes` — the `0x30` sweep target list
  (`{roller_shutter, awning, dual_shutter}` for VELUX; all-`UNKNOWN` for SOMFY).

`resolve_oneway_wire_profile()` maps manufacturer `velux` → `VELUX_KLI` + that list;
`somfy` / unset / any unprofiled vendor → `SOMFY` + empty. The exterior-shading set is
the profile default because it is the one captured from a real KLI gesture. A
per-identity `enrollment_classes:` YAML key overrides it (via
`effective_enrollment_classes()`). That is how an interior-blind identity gets
`[blind, venetian_blind]`. The classes to set are the ones the existing remote's `0x2E`
burst targets after a Gear press, visible in the DEBUG `rx 1W remote … targets …` log
lines (which keep one line per destination for intent-less frames).

`send_enrollment()` dispatches on `enroll_gesture`:

- **`SOMFY`** (`send_somfy_enrollment_()`): byte-for-byte the historical behaviour
  — `0x39` then `0x30`, both to `io_device_type`, `enrollment_with_mac` honoured.
- **`VELUX_KLI`** (`send_velux_kli_enrollment_()`): `0x39` → `00 00 3F`; then
  `send_enroll_sweep_()` reserves one sequence and bursts a `0x30` to each
  non-`UNKNOWN` class under it; then a STOP and a DOWN `CMD_EXECUTE` to `00 00 3F`
  at the VELUX ACEI, one sequence each. The gesture consumes 4 sequences total
  (`0x39`, sweep, STOP, DOWN). A failed `0x39` prelude or STOP/DOWN follow-up only
  warns; the sweep is what the return value reflects.

The schema warns when a `manufacturer: velux` + `enrollment: true` identity has an
`io_device_type` of `screen` / `blind` / `venetian_blind` and no
`enrollment_classes:` override. The default sweep ignores that `io_device_type` and
targets the exterior-shading classes, which such a device is unlikely to listen on.
The warning names the KLI 312 set and the `0x2E` lines as the fix.

### Blocking time

The VELUX gesture is 6 bursts (`0x39` + a 3-class `0x30` sweep + STOP + DOWN), and it
blocks `loop()` for the whole gesture, feeding the watchdog in the gaps. How long
depends on the identity's `low_power:` power class (ADR 0038):

- **Unset** (`LONG_PREAMBLE` on every copy): ~1.2 s per burst, ~7.4 s total measured on
  SX1276, ~10.6 s with `enrollment_with_mac: true`. That is well over ESPHome's 2550 ms
  loop-warning threshold (ADR 0013).
- **`false`** (short `normal_start_preamble` on every copy): ~200 ms per burst, ~1.2 s
  total measured on SX1276, under the threshold.
- **`true`** (one long copy, three short) sits in between.

**The exemption for the long shapes is taken knowingly**: 1W enrollment is a
user-initiated, once-per-device action, the same shape as the pairing button, whose
`pairing_discovery_wait_ms` already goes to 5000. Gesture duration is an airtime
concern, not an established cause of a failed enrollment, given the long window
after Gear.

## Consequences

- A `manufacturer: velux` enroll button emits the KLI gesture, and it has registered
  the hub as a controller on real VELUX hardware (issue #74): a VELUX SML roller shutter
  (mains-fed through a KUX 110 power supply) on SX1276 with `low_power: false` and the default sweep, and KLI 312 interior blinds
  on SX1262 with `low_power: true` and `enrollment_classes: [blind, venetian_blind]`.
  The identity's commands worked afterwards in both cases, and on the blinds the
  closing DOWN at the end of the gesture was reported as the visible success signal.
  That signal is invisible when the cover is already fully closed, which is where the
  SML shutter sat once its ready sequence had returned it.
- The per-remote class set is established for two remote families only. A KLI 311
  window remote, or any other VELUX product, may use yet another set; the `0x2E` lines
  are the way to find it rather than a table in this project.
- Which single class of a sweep a given actuator registers on is not known. Both
  confirmed enrollments swept a set.
- The STOP+DOWN frames are single-sourced (`iohc-flipper`) and not byte-matched
  against a VELUX capture, even though the gesture as a whole works.
- The Somfy enrollment path is untouched — same code, moved into a named method.
- `synthetic_enrollment_velux_kli_prog_sweep.yaml` is the golden reference for the
  exterior-shading VELUX gesture's wire shape; its STOP+DOWN frames may be corrected
  once a real VELUX enrollment is captured (hex is immutable, so that means delete +
  re-add).
- The profiled-vendor set is now written in three places (the C++ `switch`, the
  Python `_ONEWAY_WIRE_PROFILE_MANUFACTURERS`, and the schema warning) with no
  automated sync check — cross-referenced in comments, same treatment as ADR 0031.
