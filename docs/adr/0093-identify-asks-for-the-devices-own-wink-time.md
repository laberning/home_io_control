# ADR 0093: Identify asks for the device's own wink time

<!-- doxygen-label: adr0093 -->

**Status:** Accepted · **Recorded:** 2026-11

## Context

The `identify_device` action ([ADR 0006](0006-management-actions-as-native-api-actions.md)) sends the
authenticated identify request `0x1E` with a two-byte payload: the originator, then one parameter byte. The hub
sent `0xFF` there. A Somfy awning answers it with a short jog
(`tests/corpus/captures/identify/somfy_awning_identify_wrong_systemkey_sx1276.yaml`).

A VELUX roof-window actuator (type 4, subtype 1) kept jogging for more than 45 s with `0xFF`; an
ordinary close did not end it, only a stop did. With `0x02` it jogged and returned to its start
position by itself ([issue #241](https://github.com/laberning/home_io_control/issues/241)).

The public VELUX KLF 200 API (version 3.18, section 10.4.1, table 194) defines the wink time of an
identify request: 0 stops a running wink, 1 to 253 are seconds, 254 is a wink time the actuator
chooses and can differ per actuator, 255 is "wink forever". The table describes the gateway's API, not
the radio frame; that the parameter byte of `0x1E` carries this value is our reading, and both results
of issue #241 fit it.

A native API action has no optional arguments: Home Assistant makes every declared argument required.

## Decision

- **The identify request carries a wink time, and the default is 254**, the actuator's own. Every
  device then identifies for the time its maker chose. 255 is never sent.
- **`identify_device` keeps its one field** (`device_id`) and sends the default.
- **A second action, `identify_device_for`**, takes `device_id` and `seconds`: 1 to 253 identify for
  that long, 0 stops a running identify. Any other value is refused before anything is sent.
- **Until a device confirms them**, 254 and 0 are experimental in the docs and the Doxygen.

## Consequences

- An identify ends by itself on a device that honours wink time 254. On a VELUX window a bounded
  value ended the identify (issue #241); 254 itself is not yet confirmed there.
- Existing automations that call `identify_device` keep working unchanged.
- A device that ignores 254 is not known yet. If one shows up, the default becomes a fixed number of
  seconds in a new record.
- The synthetic identify capture changes its parameter byte and CRC; the recorded awning capture keeps
  `0xFF`, which is what was sent.

## Considered options

- **Keep `0xFF`.** Rejected: on a VELUX window it never ends.
- **A fixed default in seconds (for example 2).** Rejected as the default: it overrides the maker's
  choice on every device. Kept as the fallback for a device that ignores 254.
- **An optional `duration` field on `identify_device`.** Not possible: a native API action has no
  optional arguments, and a required one would break every existing call.
- **A separate stop action.** Rejected: `seconds: 0` on the second action does the same with one action
  fewer.
