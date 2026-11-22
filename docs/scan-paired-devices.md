# Scan Paired Devices
<!-- doxygen-label: scan_paired_devices -->

Once you hold a system key — recovered from another hub, or established by pairing — a roll-call
lists every device that trusts it, whether or not you have a YAML entity for it. Each device you
have no entity for answers with a ready-to-paste snippet, and no pairing handshake is involved.
This is the fastest way to bring up an installation after [key extraction](key-extraction.md).

Enable it from the hub block, not as a `button:` entry:

```yaml
home_io_control:
  # ... radio pins, node_id, system_key ...
  scan_paired_devices_button: true
```

The button and the `scan_paired_devices` action run the same roll-call. The action is always
available; the button saves a trip to Developer Tools.

## The `scan_paired_devices` action

It takes no fields. Like the other actions it is named `esphome.<node_name>_scan_paired_devices`;
for a config with `name: hioc-heltec-v2`:

```yaml
action: esphome.hioc_heltec_v2_scan_paired_devices
```

The hub broadcasts a roll-call (`CMD_DISCOVER_SPE_REQ`, 0x2A) and listens for every device that
holds its system key. Responders are grouped into a `Known:` section (already in your YAML) and an
`Unknown:` section, and each unknown one gets a paste-ready block. Either section is omitted when
it would be empty. Each line names the device as `<type>, <subtype>` (for example
`electrical heater, with adjustable setpoint`, or `subtype N` when the subtype has no name; the
[node types table](device-types.md#node-types-and-subtypes) lists them). A responder whose type
no platform drives yet gets `not supported yet` in place of the block. A reply whose signal strength
the radio could not take from the frame itself shows `rssi=n/a`
([ADR 0112](adr/0112-a-frames-rssi-is-its-own-or-unknown.md)).

```
Roll-call: 2 devices detected (1 known, 1 unknown)
Known:
  30E1F2: horizontal awning (0x10/0) rssi=-52dBm manufacturer=Somfy turnaround=40ms power_save=always_alive [known]
Unknown:
  415CE4: light (0x06/0) rssi=-61dBm manufacturer=Somfy turnaround=40ms power_save=always_alive [unknown]
    Paste this into your YAML to register it:
  light:
  - platform: home_io_control
    name: "My Device"
    io_device_id: "415CE4"
    io_device_type: "light"
    io_subtype: 0
```

## What a scan does and does not do

- **A scan takes about 13 seconds** and blocks the ESPHome loop for that long. Sleeping
  solar/battery devices need a long wake-up call that a mains-powered device ignores, so the hub
  calls each power class in turn on all three radio channels: a low-power pass first (a long
  wake-up preamble), then the always-alive pass (a short preamble) — six broadcasts total, each
  with a full `pairing_discovery_wait_ms` listen window. The "operation took a long time" warning
  ESPHome logs on every run is expected.
- **Each per-device key adds a full sweep.** A device that keeps a key of its own (the
  [`system_key`](configuration/index.md) option on its entity) only answers a roll-call built with
  that key. The scan therefore runs the six broadcasts once with the hub key, then once for every
  different per-device key, and logs `Roll-call with key check value ABC123 (n registered devices)`
  before each sweep. A device found under a per-device key shows `key=ABC123` (the key's check
  value, never the key) on its line. An unknown device found that way gets a snippet with its
  `system_key:` in the **device log only**, inside a `DEVICE KEY BELOW -- DO NOT SHARE IT` block:
  move the key into `secrets.yaml` before you post the log anywhere. The
  `esphome.home_io_control_action_result` event that Home Assistant records carries the same report
  without the key. A known device that answers under a different key than its YAML uses gets a
  `hint:` naming both key check values; fix that entity's `system_key:`.
- **An installation with no solar or battery devices can skip the low-power pass.** Set
  [`scan_power_classes: always_alive`](configuration/tuning.md#scan_power_classes) for a
  single-pass scan of about 6 seconds. The same tunable also isolates one class when bisecting a
  mixed installation in the field.
- **A device can still be missed. Press again if one you expect is absent.** A scan has no side
  effects, so repeating it costs nothing.
- **Hearing nothing is a valid result.** The result event's `success` is `true` whenever the
  broadcast went out. Its `device_id` is always empty, because there is no single target, and the
  full report above is the event's `message`.
- **At most 24 devices are listed per scan.** If more answer, the report says so with a
  `NOTE: more than 24 devices answered; the list below is truncated.` line rather than silently
  showing a subset.
- **An unknown responder is almost never an intruder.** It usually means a device you paired
  earlier whose YAML entry was never saved. The reply proves only that the device once received
  your system key.
- **A known device can get a `hint:` line** when its self-reported power-save class disagrees with
  its registered `low_power:` YAML property — for example, a device that answers the low-power pass
  but has no `low_power: true` in its YAML. Follow the hint's suggestion; it only ever proposes a
  YAML edit, never changes the registry itself.
- **It cannot pair a new device.** A device in learning mode holds no key yet and stays silent to a
  roll-call. Use [Discover & Pair](pairing.md) for that, and the scan to check on devices you
  already have.
  [`pairing_discovery_commands`](configuration/tuning.md#pairing_discovery_commands) explains why
  0x2A is deliberately not a discovery option.
- **A scan puts roughly 1.9 seconds of traffic on air** (the same on every board), dominated by the
  low-power pass's long wake-up preambles ([`low_power_wake_ms`](configuration/tuning.md#low_power_wake_ms)). It is a manually triggered action — do not call it from a tight automation
  loop.

## The Scan Paired Devices button

- The button defaults to the `config` entity category.
- There is no companion result sensor. Output goes to the log and to the
  `esphome.home_io_control_action_result` event, exactly like the action.
- The button shows as pending in Home Assistant for the duration of the scan.
- A press is ignored if a radio exchange is already in flight. That can only happen from an ESPHome
  automation pressing the button inside another entity's callback, never from a tap in the Home
  Assistant UI. The result event still fires with `success: false`, so an automation sees the
  rejection.

## See also

- [Key extraction](key-extraction.md) — how to obtain the key of an existing installation
- [Pairing](pairing.md) — Discover & Pair, for a device no hub has claimed
- [A device is missing from the scan](troubleshooting.md#a-device-is-missing-from-the-scan)
