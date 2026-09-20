# ADR 0038: 1W bursts follow the identity's power class, not a hard-coded preamble

<!-- doxygen-label: adr0038 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

Every 1W transmit — every command, position, both enrollment gestures, and un-enrollment — goes out
through `OneWayTransmitter::send_burst()`, which sends `ONEWAY_BURST_REPEATS` (4) identical copies.
That burst was hard-coded to the 1024-byte `LONG_PREAMBLE` on every copy, with `CTRL1` left at
`0x00`.

ADR 0029 established for 2W that an awake VELUX receiver does not accept a frame behind that
preamble, and accepts the same frame behind a short one — the preamble length, not the
`CTRL1_LOW_POWER` flag, is what decides. 1W never received the same treatment, and a mains-fed
VELUX roller shutter would not respond to 1W commands at all (issue #74).

Two observations about real remotes bound the design:

- A real KLI remote's GEAR and EXECUTE bursts carry the wake flag on copy 1 only, with short,
  flag-less repeats. A long preamble on that first copy is consistent with the capture's timing
  rather than visible in it. Either way, real hardware treats "wake up" as a property of one copy,
  not of the whole burst.
- A real KLI pairing gesture (`0x39` plus the `0x30` class sweep) is entirely short and flag-less;
  its inter-frame spacing physically excludes a ~213 ms preamble on any copy.

## Options considered

- **Default every identity to `low_power: false`** (parity with the 2W key of the same name).
  Rejected at the time: it changes the on-air shape of the hardware-validated Somfy path, which has
  no regression run behind it. Still the intended end state.
- **Drop the preamble to `normal_start_preamble` globally, with no per-identity key.** Same
  objection, with no opt-out for a genuinely duty-cycled receiver.
- **Tri-state key, legacy default** (chosen). Existing identities keep their exact bytes; an
  identity under test opts into a new shape by setting `low_power:` at all.
- **A key name distinct from the 2W `low_power`.** Rejected: it is the same underlying question —
  does this receiver need a wake-up preamble — and a second name would imply a second mechanism.

## Decision

`OneWayControllerIdentity` carries a `OneWayPowerClass` (`oneway_controller.h`), driven by the
identity's `low_power:` YAML key. The key is **tri-state** here: unlike the plain-bool 2W key,
*unset* is its own state.

| `low_power:` | `OneWayPowerClass` | Copy 1 | Copies 2–4 |
|---|---|---|---|
| unset | `LEGACY_LONG` | `LONG_PREAMBLE`, CTRL1 `0x00` | `LONG_PREAMBLE`, CTRL1 `0x00` |
| `false` | `ALWAYS_ALIVE` | `normal_start_preamble`, CTRL1 `0x00` | `normal_start_preamble`, CTRL1 `0x00` |
| `true` | `LOW_POWER` | `LONG_PREAMBLE`, `CTRL1_LOW_POWER` | `normal_start_preamble`, CTRL1 `0x00` |

- A pure function, `oneway_burst_copy_shape(power_class, copy_index)`, implements this table. It is
  chip-neutral and knows nothing about `TuningConfig` — it names a preamble *class* (`WAKE` /
  `NORMAL`), not a byte count.
- `OneWayTransmitter` takes a `const TuningConfig *` (the pattern `ExchangeEngine` already uses) and
  resolves `NORMAL` to `tuning_->normal_start_preamble` inside `send_burst()`, read fresh on every
  burst, so the Home Assistant tuning entity takes effect without a reboot.
- `send_burst()` takes the power class as a required parameter. No default: a default would
  silently reintroduce a hard-coded preamble at a new call site, the exact mistake ADR 0029 names.
  It builds each copy from the frame the caller signed, toggling only `CTRL1_LOW_POWER` and the
  preamble — cmd, data, sequence and MAC are untouched. That is safe because the 1W MAC span covers
  only cmd+data, never CTRL1, and the driver computes the CRC per transmission.
- The class applies to **every** transmit of the identity: `send_command()`, `send_position()`,
  `send_enrollment()` (both gestures, including the VELUX `0x39` prelude and the STOP/DOWN
  follow-up), and `send_unenrollment()`. A partial application would let one identity behave two
  ways depending on which button was pressed.
- The TX log prints the actual bytes per copy (`preamble 1024/32/32/32`), built from the same
  `oneway_burst_copy_shape()` the transmit loop calls, so it cannot claim a shape that was not sent.
  The boot log prints each identity's resolved class, unset included.

## Consequences

- **Unset knowingly violates ADR 0029's "flag and preamble never disagree" invariant.** A
  `LEGACY_LONG` copy carries `LONG_PREAMBLE` with `CTRL1_LOW_POWER` clear — the mismatch ADR 0029
  calls recognisable on sight as the mistake. This is a deliberate, recorded exception for
  back-compatibility with the Somfy path, not a new general pattern.
- **Unset is not equivalent to `false`, unlike the 2W key of the same name.** This has to be
  documented everywhere the key appears, or a user reasonably assumes they behave the same way.
- **The unset default cannot work on an awake VELUX receiver, and fails silently.** 1W carries no
  acknowledgement, so a burst the receiver ignores is indistinguishable in our logs from one it
  obeys. Issue #74 established this directly: on a mains-fed VELUX roller shutter, a burst with all
  four copies at 1024 bytes produced no movement, while both shapes putting any copy at 32 bytes
  moved it, on the same link and identity, with `CTRL1` held at `0x00` throughout. Whether the
  default should stay `LEGACY_LONG` for every manufacturer is therefore open; changing it for the
  Somfy path needs a regression run on Somfy hardware first.
- **`low_power: true` keeps a wake copy on every burst of the enrollment gesture**, though a real
  KLI's own pairing gesture is entirely short and flag-less. This is the conservative choice and
  matches what a real KLI's GEAR/EXECUTE bursts do.
- **One tuning value, `normal_start_preamble`, now also governs 1W bursts.** A user needing a longer
  value for a marginal 2W link and a shorter one for 1W cannot have both. That was already true of
  the 2W directed start frame and the broadcast roll-call (ADR 0029), so 1W joining them is
  consistent rather than a new limitation.

See also [ADR 0029](0029-start-preamble-is-a-property-of-the-target.md) (the 2W precedent this
mirrors) and [ADR 0032](0032-oneway-velux-enrollment-gesture.md) (the VELUX enrollment gesture this
governs).
