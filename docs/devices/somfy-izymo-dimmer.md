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
- **A 1W Smoove remote can be enrolled onto it**, and this was captured both with and without the
  MAC-bearing form of the enrollment frame. If your own enrollment does not take, `enrollment_with_mac`
  is the option to flip — see [Sending 1W commands](../configuration/oneway-transmit.md).
- **Probes return data that is not decoded.** `CMD_GET_INFO2`, `CMD_GET_GENERAL_INFO3` and the
  private-function reads all answer, but nothing in those replies has been mapped to a useful
  value yet.

## Evidence

39 corpus captures on the maintainer's own hardware, nodes `1DAF16` and `4200E0` — the largest
single-device set in the corpus. They cover pairing, on/off, brightness at 50% and 75%, status
polls for each of those states, roll-call discovery, 1W enrollment, and six probe scenarios, across
all three radio chips.

## See also

- [Lights, locks and switches](../configuration/light-lock-switch.md)
- [Supported devices](../supported-devices.md)
