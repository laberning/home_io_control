# ADR 0038: 1W bursts follow the identity's power class, not a hard-coded preamble
<!-- doxygen-label: adr0038 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

Every 1W transmit — every command, position, both enrollment gestures, and un-enrollment — goes
out through `OneWayTransmitter::send_burst()`, which sends `ONEWAY_BURST_REPEATS` (4) identical
copies with the 1024-byte `LONG_PREAMBLE` on every copy and `CTRL1` left at 0x00. That shape has
never been configurable.

ADR 0029 proved on hardware, for 2W, that this exact preamble stops an always-alive (mains-powered)
VELUX receiver from taking a directed frame at all — not the `CTRL1_LOW_POWER` flag, the preamble
length itself. It introduced a per-device `low_power:` key: `false` (the default) sends a directed
start frame at the shorter, runtime-tunable `normal_start_preamble`; `true` keeps `LONG_PREAMBLE`
and sets the flag. 1W never received the same treatment. In issue #74 a mains-fed VELUX SML roller shutter
(powered through a KUX 110) did not 1W-enroll with the long preamble on every copy, which makes the same mechanism a plausible
blocker for 1W too.

Two more data points shape the decision:

- A real KLI remote's GEAR/EXECUTE bursts carry the wake flag on copy 1 only, with short, flag-less
  repeats — established from a capture. A long preamble specifically on that first copy is not
  observed in the capture, only consistent with it: the capture's timing leaves room for one on the
  first copy. Either way, real hardware already treats "wake up" as a copy-1 property, not a
  whole-burst property.
- A real KLI pairing gesture (`0x39` + the `0x30` class sweep) is entirely short and flag-less — its
  inter-frame spacing physically excludes a ~213 ms preamble on any copy.

## Options considered

- **Default every identity to `low_power: false`** (parity with the 2W key). Rejected for now: it
  would change the on-air shape of the hardware-validated Somfy path without a regression run on
  real Somfy hardware first.
- **Drop the preamble to `normal_start_preamble` globally, no per-identity key.** Same objection —
  an unreviewed change to a path this project has actually validated on real actuators.
- **Tri-state key, legacy default** (chosen). Every existing identity keeps today's bytes exactly;
  an identity a user is actively testing opts in to the new shape by setting `low_power:` at all.
- **A new key name distinct from the 2W `low_power`.** Rejected: it is the same underlying concept
  (does this receiver need a wake-up preamble), and a different name for it would suggest a
  different mechanism where none exists.

## Decision

`OneWayControllerIdentity` gains `OneWayPowerClass power_class` (`oneway_controller.h`), driven by
the identity's `low_power:` YAML key, which is **tri-state** here — unlike the plain-bool 2W key,
*unset* is its own state:

| `low_power:` | `OneWayPowerClass` | Copy 1 | Copies 2–4 |
|---|---|---|---|
| unset | `LEGACY_LONG` | `LONG_PREAMBLE`, CTRL1 `0x00` | `LONG_PREAMBLE`, CTRL1 `0x00` |
| `false` | `ALWAYS_ALIVE` | `normal_start_preamble`, CTRL1 `0x00` | `normal_start_preamble`, CTRL1 `0x00` |
| `true` | `LOW_POWER` | `LONG_PREAMBLE`, `CTRL1_LOW_POWER` | `normal_start_preamble`, CTRL1 `0x00` |

- A pure function, `oneway_burst_copy_shape(power_class, copy_index)`, implements this table. It is
  chip-neutral and knows nothing about `TuningConfig` — it names a preamble *class* (`WAKE` /
  `NORMAL`), not a byte count.
- `OneWayTransmitter` takes a `const TuningConfig *` in its constructor (the same pattern
  `ExchangeEngine` already uses) and resolves `NORMAL` to `tuning_->normal_start_preamble` inside
  `send_burst()`, read fresh on every burst — never cached — so the Home Assistant tuning entity
  takes effect on the next command without a reboot.
- `send_burst()` now takes the power class as a required parameter (no default: a default would
  silently reintroduce a hard-coded preamble at a new call site, the exact mistake ADR 0029 names).
  It builds each copy from the frame the caller signed, toggling only `CTRL1_LOW_POWER` and the
  preamble — cmd/data/sequence/MAC are untouched. This is safe because the 1W MAC span covers only
  cmd+data (never CTRL1) and the CRC is computed per transmission by the driver.
- The class applies to **every** TX of the identity: `send_command()`, `send_position()`,
  `send_enrollment()` (both gestures, including the VELUX `0x39` prelude and STOP/DOWN follow-up),
  and `send_unenrollment()`. A partial application (e.g. commands but not enrollment) would let one
  identity behave two different ways depending on which button was pressed.
- The TX log line prints the actual preamble bytes per copy, e.g. `preamble 1024/32/32/32`, built
  from the same `oneway_burst_copy_shape()` the transmit loop calls — so the log can never claim a
  shape different from what was sent. The boot log prints the resolved class for every identity,
  unset included, via `oneway_power_class_name()`.

## Consequences

- **Unset knowingly violates ADR 0029's "flag and preamble never disagree" invariant.** A
  `LEGACY_LONG` copy carries `LONG_PREAMBLE` with `CTRL1_LOW_POWER` clear — exactly the mismatch
  ADR 0029 calls "recognisable on sight as the mistake". This is a deliberate, recorded exception
  for back-compat with the hardware-validated Somfy path, not a new general pattern; a later change
  that moves the default to `false` for all identities (ADR 0029 parity) needs its own Somfy
  regression run first, tracked as a follow-up.
- **Unset is not equivalent to `false`, unlike the 2W key of the same name.** This must be
  documented everywhere the key appears, or a user reasonably assumes the two behave the same way.
- **`low_power: true` enrollment frames keep a wake copy on every burst of the gesture**, even
  though a real KLI's own pairing gesture is entirely short and flag-less. This is the conservative
  choice (matches what a real KLI's GEAR/EXECUTE bursts do, and what another open-source 1W
  transmitter does for its wake-up copy). It has enrolled VELUX interior blinds (KLI 312) on SX1262
  in issue #74.
- **One tuning value, `normal_start_preamble`, now also governs 1W bursts.** A user who needs a
  longer value for a marginal 2W link and a shorter one for 1W has no way to have both; this was
  already true of the 2W directed start frame and the broadcast roll-call (ADR 0029), so 1W joining
  them is consistent, not a new limitation.
- **Both non-legacy classes work on VELUX hardware; the cause of the legacy failure is not
  isolated.** In issue #74, `false` enrolled and controls a mains-fed SML roller shutter on SX1276 (the whole
  enrollment gesture takes ~1.2 s instead of ~7.4 s), and `true` enrolled interior blinds on SX1262.
  The SML success also changed the user's registration procedure and the gesture's duration at
  the same time, so it does not by itself prove the all-long preamble was what blocked the earlier
  attempt. Whether the unset default should change for VELUX identities waits on a
  commands-only A/B on an already-enrolled identity.

See also ADR 0029 (the 2W precedent this mirrors) and ADR 0032 (the VELUX enrollment gesture this
now also governs, including its blocking time).
