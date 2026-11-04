# ADR 0065: A cover's travel speed is a three-way choice

<!-- doxygen-label: adr0065 -->

**Status:** Accepted · **Recorded:** 2026-11

## Context

An execute frame in the 8-byte form ends in a speed profile byte. The hub sends `06` on every
position move to a kind that takes that form, and `05` for `silent: true`: the two values a Somfy
hub sends for a normal and for a silent move. A VELUX controller sends neither. A KLR 200 control
pad moves a window with the 6-byte form, `01 63 C8 00 00 00` for a close, which carries the main
parameter only.

On a VELUX roof window the three frames give three speeds. A full travel of one window, measured in
issue #205: 44.5 s from its pad, 26.5 s on profile `06` and about 65 s on profile `05`. So the
hub moved the window at its fast speed, audibly louder than the pad, and a window driven by both
moved at two speeds. The VELUX KLF 200 API names three velocities for a command, DEFAULT (0),
SILENT (1) and FAST (2); `05` acting as SILENT and `06` as FAST fits them.

`silent:` is a boolean, and its switch chooses between `05` and `06`. Nothing chose the 6-byte form.

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
2. **Without the key the hub sends what it sent before**: profile `06`, or what `silent:` says.
   `silent:` stays as the two-way form of the same setting: `true` is `silent`, `false` is `fast`. A
   cover takes one of the two keys.
3. **Declaring the key generates a `<Cover Name> Travel Speed` select** with the three options, and
   the YAML value is its boot state, as for the Silent Operation switch. The device holds a
   `TravelSpeed` in place of its `silent` flag. The enumerator for the motor's own speed is
   `MOTOR_DEFAULT`, not `DEFAULT`, which the Arduino core defines as a macro on some targets.
4. **The speed shapes position moves**, open and close included. The favorite position keeps its
   6-byte frame except on `silent`, as before. Stop, force open, ventilation and tilt are unchanged.

## Consequences

- No existing config changes behaviour or entities.
- A VELUX window can move at its pad's speed, and then moves alike from the pad and from the hub.
- What `default` does on other makers' motors is unmeasured. Kinds without the 8-byte form already
  get the same 6-byte frame for every move, so a motor is expected to accept it.
- Like `silent:`, `travel_speed:` is a declared preference: the select shows what the hub sends, not
  a reading from the device.
