# Somfy RS100 IO
<!-- doxygen-label: dev_somfy_rs100 -->

A solar-powered awning and shutter actuator. It works, but its pairing is the least reliable of any
confirmed device here, and the reason is worth understanding before you start.

## What works

| Capability | Status |
|---|---|
| Open, close, stop | ✅ Confirmed |
| Position feedback | ✅ Confirmed |
| Discover & Pair | ⚠️ Partial — the key exchange often needs a retry |
| Status poll while driven by another hub | 🔍 Captured, from a KIG 300 installation |

## Onboarding route that worked

**Discover & Pair, retried.** The key-exchange step is what stalls, not discovery: the device
answers the discovery, then the key transfer times out. Retrying the same button press has worked.
The corpus holds both outcomes side by side — a successful retry and a stalled transfer — so this
is a known, characterised failure rather than an unexplained one.

If it will not pair after several attempts and the motor has ever belonged to a hub you still own,
switch to [key extraction](../key-extraction.md).

## Working YAML

```yaml
cover:
  - platform: home_io_control
    name: "Terrace Awning"
    device_class: awning
    io_device_id: "…"
    io_device_type: "awning"
    io_subtype: 0
    low_power: true
    status_poll_interval: 2s
```

## Known quirks

- **`low_power: true` is the setting to start with.** This is a solar actuator, so it duty-cycles
  its receiver and needs the long wake-up preamble. Be aware this is a reasoned starting point
  rather than a confirmed fix: the option is documented for battery and solar devices generally,
  and it is confirmed on VELUX hardware through issue #87, but no field report has yet said in so
  many words that setting it fixed an RS100. If you try it either way, that is worth an issue.
- **Range matters more than on a mains-powered motor.** The corpus has near, far and failure
  captures at different distances against the same device, and the failure case is a range case.
- **The Oximo 40 Solar is the same story.** It appears in the corpus only as overheard status
  traffic, but it is the same solar class and the same `low_power:` advice applies.

## Evidence

7 corpus captures from field reports: a successful key-exchange retry, a key-transfer timeout, near
/ far / failure exchanges on SX1262, a status poll while the device was controlled by a VELUX KIG
300 hub, and a discovery attempt alongside 1W traffic.

Field reports: issues #16 and #45.

## See also

- [Covers](../configuration/cover.md)
- [Troubleshooting](../troubleshooting.md#the-device-is-never-found)
