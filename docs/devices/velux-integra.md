# VELUX INTEGRA and the KLR/KLF/KUX family
<!-- doxygen-label: dev_velux_integra -->

One landing page for the VELUX side of the ecosystem: the roof-window actuators, the control pads
and hubs that make good key sources, and the wired bridges that do not currently work.

## What works

| Device | Status |
|---|---|
| INTEGRA roof-window actuator | ✅ Confirmed — open, close, position, ventilation |
| KLR 200 two-way control pad | ✅ Confirmed as a key-extraction source |
| KIG 300 hub | ⚠️ Partial — one extraction succeeded, one stalled |
| Devices behind an extracted key | ✅ Confirmed, with `low_power: true` |
| MSU 100100 solar awning screen | ⚠️ Partial — open and close work, `stop` is ignored |
| INTEGRA SOLAR blinds | 📣 Reported, no capture |
| SML shutter via KUX 110 | ❌ Nothing answers discovery |
| KUX 110 / KUX 100 1W enrollment | ❌ Enrollment transmits, the device never reacts |

## Onboarding route that worked

**Key extraction, essentially always — when there's a two-way hub.** VELUX installations almost
all have a control pad or hub already, and that hub is what you extract from. Against a KLR 200
this succeeded end to end on the first attempt, including the address round.

For an installation with no hub at all — only a 1W wall switch or remote, or a device fresh out of
the box — key extraction has nothing to extract from. See the SSL entry under Known quirks below
for the best currently-known route in that case.

Once you hold the key, press **Scan Paired Devices** rather than pairing each device. Every device
that trusts the key answers with a ready-to-paste snippet.

## Working YAML

```yaml
home_io_control:
  # ... radio pins ...
  node_id: "810BAB"        # recovered from the KLR 200
  system_key: "…"          # recovered from the KLR 200
  scan_paired_devices_button: true

cover:
  - platform: home_io_control
    name: "Roof Window"
    device_class: window
    io_device_id: "6544C6"
    io_device_type: "window_opener"
    io_subtype: 0
    low_power: true
```

`io_device_type: "window_opener"` is what generates the **Ventilation Position** button, which
moves the window to its predefined air-exchange opening rather than fully open.

## Known quirks

- **`low_power: true` is usually required.** VELUX actuators are frequently solar or battery
  powered and sleep between commands. Without this the hub addresses them with the wrong preamble.
  In issue #87 a roll-call after a successful key extraction drew no replies at all until the
  devices were declared `low_power: true`.
- **The MSU solar awning screen ignores `stop` mid-motion.** It will not complete a 2W handshake
  while it is travelling, so there is nothing for the stop command to talk to. Open and close both
  work. Reported in issue #95; there is no hub-side fix, and the 1W route needs enrollment that
  does not currently take on this family.
- **1W enrollment into the KUX bridges does not work yet.** The hub transmits the gesture and the
  device never reacts. Tried with the `roller_shutter` and `dual_shutter` classes; the `awning`
  class has not been tried. Reported in issue #74.
- **VELUX uses a different 1W enrollment gesture from Somfy**, and a different priority byte on
  `CMD_EXECUTE`. Set `manufacturer: velux` on any 1W identity aimed at these devices — see
  [Sending 1W commands](../configuration/oneway-transmit.md).
- **An SSL solar roller shutter ships pre-paired, even if it has never met a hub.** Its included
  KLI 313 wall switch comes pre-configured and the shutter pre-paired to it, per VELUX's own
  datasheet. "Never connected to a hub" is not the same as "factory fresh" — a plain Discover &
  Pair attempt finds nothing because the device already holds a 1W pairing, not because it is
  unreachable.
- **The SSL has a documented physical reset, but it's easy to miss.** Older SSL installation
  manuals (VAS 453334-2013-08 and similar) describe a
  hidden **P** button under the shutter's top-casing cover: set the nearby slider switch to **I**,
  wait 10 seconds, then hold **P** until the shutter buzzes three times. This opens a **10-minute**
  registration window and is re-armable — pressing P again after the window lapses opens a fresh
  one. The manual only documents this as the path to re-pair the 1W wall switch; whether the same
  window also admits a 2W Discover & Pair attempt is untested yet.

## Evidence

Corpus captures across the family: 3 for the INTEGRA roof-window actuator (node `6544C6`, probe
replies, from issue #98), 1 for the successful KLR 200 key extraction (node `810BAB`, issue #80),
4 for the KIG 300 hub (node `BEFEDB`), 3 for KLR200 ↔ KUX100 bridge traffic, and 3 for the KLI 310
and KLI 313 remotes.

Field reports: #74 (KUX enrollment), #80 (KLR 200 extraction), #87 (roll-call preamble), #95 (MSU
screen stop), #98 (INTEGRA probes).

## See also

- [Key extraction](../key-extraction.md)
- [Scan Paired Devices](../pairing.md#scan-paired-devices)
- [Troubleshooting](../troubleshooting.md#commands-are-ignored-mid-motion)
