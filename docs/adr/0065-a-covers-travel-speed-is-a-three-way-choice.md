# ADR 0065: A cover's travel speed is a three-way choice

<!-- doxygen-label: adr0065 -->

**Status:** Accepted, amended by [ADR 0069](0069-a-config-key-is-retired-in-two-stages.md) (`silent:` is deprecated); amends [ADR 0059](0059-device-capabilities-come-from-the-type-table.md) (lights, switches, locks and an undeclared heater-level device get the 6-byte frame); decision 2 and the consequences revised in place (the 6-byte form is the default) · **Recorded:** 2026-11

## Context

An execute frame in the 8-byte form ends in a speed profile byte. `06` and `05` are the two values a
Somfy hub sends for a normal and for a silent move. A VELUX controller sends neither. A KLR 200 control
pad moves a window with the 6-byte form, `01 63 C8 00 00 00` for a close, which carries the main
parameter only.

On a VELUX roof window the three frames give three speeds. A full travel of one window, measured in
issue #205: 44.5 s from its pad, 26.5 s on profile `06` and about 65 s on profile `05`. So the
hub moved the window at its fast speed, audibly louder than the pad, and a window driven by both
moved at two speeds. The VELUX KLF 200 API names three velocities for a command, DEFAULT (0),
SILENT (1) and FAST (2); `05` acting as SILENT and `06` as FAST fits them.

`silent:` is a boolean, and its switch chooses between `05` and `06`. Nothing chose the 6-byte form.

Every device in the corpus answers the 8-byte form with result code 5, a parameter of the request is
unknown (25 of 25 answers, from a Somfy awning, a Somfy dimmer on three radio chips, a VELUX roller
shutter and a tilt cover), and answers the 6-byte form with code 0 (6 of 6). They move on both. The
profile is a parameter most devices do not have.

## Options considered

- **Send the 6-byte form to every VELUX window.** Rejected: a cover declares no maker, so nothing
  tells a VELUX window from another, and the fast speed is one some users want.
- **Make `silent:` three-valued.** Rejected: a boolean key that also takes a string, and a switch
  that would have to become a select, changing the entity of every config that has one.
- **Send profile `04` for the motor's own speed**, the KLF velocity 0 with the offset `05` and `06`
  share. Not chosen: nothing has been captured with it, while the 6-byte form is what the pad sends
  and what was measured.

## Decision

1. **`travel_speed:` takes `default`, `silent` or `fast`** on a cover whose kind takes the 8-byte
   form, the same gate as `silent:`. `fast` sends profile `06`, `silent` profile `05`, and `default`
   the 6-byte main-parameter frame that `create_execute_main_parameter()` already builds for kinds
   without the 8-byte form.
2. **Without a chosen speed the hub sends the 6-byte main-parameter frame**, which leaves the speed
   to the motor. The profile goes out only when a speed is chosen, by the key, by `silent:` or by
   the select. `silent:` stays as the two-way form of the same setting: `true` is `silent`, `false`
   is `fast`. A cover takes one of the two keys. Lights, switches and locks have no speed and always
   get the 6-byte frame.
3. **Declaring the key generates a `<Cover Name> Travel Speed` select** with the three options, and
   the YAML value is its boot state, as for the Silent Operation switch. The device holds a
   `TravelSpeed` in place of its `silent` flag. The enumerator for the motor's own speed is
   `MOTOR_DEFAULT`, not `DEFAULT`, which the Arduino core defines as a macro on some targets.
4. **The speed shapes position moves**, open and close included, and force open. The favorite
   position keeps its 6-byte frame except on `silent`, as before. Stop, ventilation and tilt are
   unchanged.
5. **The hub reads the result code of the answer to a command** (bits 7 to 3 of byte 0, above the
   execution state of ADR 0079). Code 5 after a frame that carried the profile logs one warning
   per device and boot that the device takes no speed setting; any other non-zero code logs a
   warning too. Nothing else changes: the Last Result Code and Active Issue sensors keep ADR
   0049's meaning.

## Consequences

- A cover without a speed key now moves at the motor's own speed, where it moved on profile `06`.
  On a VELUX window that is the pad's speed, so it moves alike from the pad and from the hub; a
  Somfy motor, which has no speed parameter, no longer answers every command with code 5. A config
  that wants the old frame sets `travel_speed: fast`. No entity changes.
- Every command to a light, switch or lock is the 6-byte frame, so those kinds lose the capability
  bit that selects the 8-byte form, and so does an exterior-heating device of undeclared kind, which
  has no speed either.
- A device that does take a speed is not told one unless the user chooses it; its motor runs at its
  own speed.
- Like `silent:`, `travel_speed:` is a declared preference: the select shows what the hub sends, not
  a reading from the device.
