# Somfy Izymo IO dimmer
<!-- doxygen-label: dev_somfy_izymo_dimmer -->

An IO-Homecontrol in-wall light receiver with brightness control. This is the device the `light:`
platform is validated against.

## What works

| Capability | Status |
|---|---|
| On / off | ✅ Confirmed |
| Brightness (intermediate levels) | ✅ Confirmed |
| Status readback after a command | ✅ Confirmed |
| Discover & Pair | ✅ Confirmed, repeatedly, including after factory resets |
| Being driven by a linked 1W remote | ✅ Confirmed with a Somfy Smoove |
| Enrolling the hub's 1W identity, and removing it inside the PROG window | ✅ Confirmed (corpus `somfy_izymo_dimmer_enrollment_remove_controller_in_prog_window_sx1276`) |
| On / off over 1W | ✅ Confirmed — 50 of 50 commands obeyed in each burst shape on four boards, checked by a two-way read |
| Brightness over 1W | ✅ Confirmed — the one-way light entity set on, off and 25, 50 and 75 % ten times each in each way, 50 of 50, checked by a two-way read; `frame` exact, `timed` 2 points brighter (the stop burst's lag). The fade is linear, about 4.7 s each way. See [A light for the identity](../configuration/oneway-transmit.md#a-light-for-the-identity) |
| Favorite / My position | ❌ The device answers the probe, but nothing useful is decoded |

All three radios are confirmed against this device: SX1276, SX1262 and LR1121 each have their own
on, off, brightness and status-poll captures.

## Onboarding route that worked

**Discover & Pair**, straightforwardly. Put the dimmer into pairing mode by holding its PROG
button for 10 seconds; when the light flashes, release it and hold the CFG button for 10 seconds
until it flashes again.

## Working YAML

```yaml
light:
  - platform: home_io_control
    name: "Hall Light"
    io_device_id: "1DAF16"
    io_device_type: "light"
    io_subtype: 0
    dimmable: true
```

`dimmable: true` is the part that matters. Without it the entity is on/off only, because nothing on
the wire says whether a light device supports intermediate positions.

## Known quirks

- **The alternate discovery command draws no response.** A `0x2E` broadcast is not answered by this
  device, which is consistent with every other device in the corpus.
- **A 1W Smoove remote can be enrolled onto it**, and so can the hub's own 1W identity with the default
  enrollment frame, which carries no MAC ([ADR 0124](../adr/0124-enrollment-with-mac-is-removed.md)). Pressing Enroll for an
  identity the dimmer already knows takes it off instead; see
  [Sending 1W commands](../configuration/oneway-transmit.md#enrolling-this-hub-as-a-controller).
- **1W enrollment is a PROG hold, then Enroll.** Hold PROG on the dimmer, then press Enroll, with
  `manufacturer: somfy` and `enrollment: true` on the identity. With `low_power:` unset the hub sends
  the short burst shape the dimmer obeys
  ([ADR 0122](../adr/0122-an-unset-one-way-low-power-sends-four-short-copies.md)).
- **Removing an enrolled 1W controller needs the PROG window.** The `oneway_remove_controller`
  action takes effect only when you press PROG first: the light blinks and then ignores that
  controller. Sent with no window open, it changes nothing.
- **Probes return data that is not decoded.** `CMD_GET_INFO2`, `CMD_GET_GENERAL_INFO3` and the
  private-function reads all answer, but nothing in those replies has been mapped to a useful
  value yet.

## Evidence

41 corpus captures on the maintainer's own hardware, nodes `1DAF16`, `4200E0` and `87FF2F` (the
dimmer takes a new address each time it is reset) — the largest
single-device set in the corpus. They cover pairing, on/off, brightness at 50% and 75%, status
polls for each of those states, roll-call discovery, 1W enrollment and removal, and seven probe
scenarios, across all three radio chips.

## See also

- [Lights, locks and switches](../configuration/light-lock-switch.md)
- [Supported devices](../supported-devices.md)
