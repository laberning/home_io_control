# Somfy RS100 IO
<!-- doxygen-label: dev_somfy_rs100 -->

An awning and shutter actuator sold in two power classes. The RS100 Solar io duty-cycles its
receiver and runs with `low_power: true`. The mains RS100 IO listens all the time and runs without
it. The roll-call reports `power_save=always_alive` for a mains motor, which is what the setting
follows rather than the model name.

Pairing usually works on the first press; when it doesn't, the key exchange is the step that
stalls, and one motor has refused two-way altogether.

## What works

| Capability | Status |
|---|---|
| Open, close, stop | ✅ Confirmed |
| Position feedback | ✅ Confirmed |
| Mains motor without `low_power:` | ✅ Confirmed on five RS100 IO |
| Discover & Pair | ⚠️ Partial — often first try; the key exchange can stall and need a retry |
| Status poll while driven by another hub | 🔍 Captured, from a KIG 300 installation |
| Battery level (`battery: true`) | ⚠️ Coarse level only (full / normal / low); only `full` observed so far |
| Sun level and battery percentage (`sun_level_poll_interval`, `battery_level_poll_interval`) | ⚠️ Experimental — read on one RS100 Solar io (issue #156); the sun sensor reads in lux with an estimated scale of 100 lux per step, to be confirmed with a lux meter |

## Onboarding route that worked

**Discover & Pair, with `low_power: true`.** Two RS100 Solar io paired on the first press on a
Heltec V3.2 (issue #156), and another on a Heltec V3 (issue #187). When it fails, the device has
answered the discovery and the key transfer then times out. Pressing Discover & Pair again has
worked; the corpus holds a successful retry and a stalled transfer side by side.

If it will not pair after several attempts and the motor has ever belonged to a hub you still own,
switch to [key extraction](../key-extraction.md).

**A motor a TaHoma holds can also be paired by address.** One mains RS100 IO that held a TaHoma's key
answered no discovery, with its learn window open or not. It took a key pushed directly to its
address in its power-cycle learn window and obeyed authenticated commands afterwards (pull request
#257). Declare the motor in your YAML and run
[Pairing a device by address](../pairing.md#pairing-a-device-by-address); the TaHoma loses the
motor. This is confirmed on that one unit.

**If the motor answers discovery but nothing after it**, retries won't help. One RS100 Solar io
answered every discovery and then ignored every frame addressed to it, through key retries,
longer and shorter preambles and a cleared low-power flag, while an identical motor next to it
paired normally (issue #187). It still obeys one-way commands: enroll the hub as a remote, see
[Sending 1W commands](../configuration/oneway-transmit.md#enrolling-this-hub-as-a-controller).
You get open, close and stop, without position feedback.

## Working YAML

A solar motor:

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

A mains motor, which reports itself as a roller shutter:

```yaml
cover:
  - platform: home_io_control
    name: "Living Room"
    io_device_id: "…"
    io_device_type: "roller_shutter"
    io_subtype: 0
    status_poll_interval: 2s
```

## Known quirks

- **Set `low_power: true` on a solar motor and leave it off on a mains one.** A solar actuator
  needs the long wake-up preamble, and every RS100 Solar io that paired in a report ran with it
  (issues #156, #187). On five mains motors the flag costs exchanges: ten boot reads took 18
  attempts and two of them failed with it, where without it all ten were answered on the first try
  in 35 to 38 ms. Scan Paired Devices prints a hint when a device that reports
  `power_save=always_alive` is configured with the flag.
- **A mains motor reports no remaining travel time.** The hub cannot schedule a read at the end of
  travel, so the position refreshes every 3 s, or at `status_poll_interval` when you set one. A
  long interval leaves the cover shown as moving until the interval elapses.
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

Five mains RS100 IO roller shutters on a LilyGO T-Beam v1.2 / SX1276 cover the always-alive rows
(pull request #258).

Field reports: issues #16, #45, #156 and #187.

## See also

- [Covers](../configuration/cover.md)
- [Troubleshooting](../troubleshooting.md#the-device-is-never-found)
