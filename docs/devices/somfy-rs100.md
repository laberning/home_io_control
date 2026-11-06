# Somfy RS100 IO
<!-- doxygen-label: dev_somfy_rs100 -->

A solar-powered awning and shutter actuator. It pairs and runs over two-way with `low_power: true`.
Pairing usually works on the first press; when it doesn't, the key exchange is the step that
stalls, and one motor has refused two-way altogether.

## What works

| Capability | Status |
|---|---|
| Open, close, stop | ✅ Confirmed |
| Position feedback | ✅ Confirmed |
| Discover & Pair | ⚠️ Partial — often first try; the key exchange can stall and need a retry |
| Status poll while driven by another hub | 🔍 Captured, from a KIG 300 installation |
| Battery level (`battery: true`) | ⚠️ Coarse level only (full / normal / low); only `full` observed so far |
| Sun level and battery percentage (`sun_level_poll_interval`, `battery_level_poll_interval`) | ⚠️ Experimental — read on one RS100 Solar io (issue #156); the sun value has no known unit |

## Onboarding route that worked

**Discover & Pair, with `low_power: true`.** Two RS100 Solar io paired on the first press on a
Heltec V3.2 (issue #156), and another on a Heltec V3 (issue #187). When it fails, the device has
answered the discovery and the key transfer then times out. Pressing Discover & Pair again has
worked; the corpus holds a successful retry and a stalled transfer side by side.

If it will not pair after several attempts and the motor has ever belonged to a hub you still own,
switch to [key extraction](../key-extraction.md).

**If the motor answers discovery but nothing after it**, retries won't help. One RS100 Solar io
answered every discovery and then ignored every frame addressed to it, through key retries,
longer and shorter preambles and a cleared low-power flag, while an identical motor next to it
paired normally (issue #187). It still obeys one-way commands: enroll the hub as a remote, see
[Sending 1W commands](../configuration/oneway-transmit.md#enrolling-this-hub-as-a-controller).
You get open, close and stop, without position feedback.

## Working YAML

```yaml
cover:
  - platform: home_io_control
    name: "Terrace Awning"
    io_device_id: "…"
    io_device_type: "vertical_exterior_awning"
    io_subtype: 0
    low_power: true
    status_poll_interval: 2s
    battery: true
```

## Known quirks

- **Set `low_power: true`.** This is a solar actuator: it duty-cycles its receiver and needs the
  long wake-up preamble. Every RS100 that paired in a report so far ran with it (issues #156,
  #187). No report has compared it against `false` on the same motor.
- **Range matters more than on a mains-powered motor.** The corpus has near, far and failure
  captures at different distances against the same device, and the failure case is a range case.
- **The battery level in status replies is coarse.** The motor reports full, normal or low, and
  only `full` has been seen so far — see [Battery level](../configuration/cover.md#battery-level).
  An RS100 Solar io also answers a request for its battery percentage and for its sun sensor's
  value; both are experimental sensors, see
  [Sun level and battery level](../configuration/cover.md#sun-level-and-battery-level) (issue #156).
- **The Oximo 40 Solar is the same story.** It appears in the corpus only as overheard status
  traffic, but it is the same solar class and the same `low_power:` advice applies.

## Evidence

7 corpus captures from field reports: a successful key-exchange retry, a key-transfer timeout, near
/ far / failure exchanges on SX1262, a status poll while the device was controlled by a VELUX KIG
300 hub, and a discovery attempt alongside 1W traffic. Three more captures hold an RS100 Solar
io's answers to the sun level and battery level requests (`somfy_rs100_probe_subscribe_event_*`).

Field reports: issues #16, #45, #156 and #187.

## See also

- [Covers](../configuration/cover.md)
- [Troubleshooting](../troubleshooting.md#the-device-is-never-found)
