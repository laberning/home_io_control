# VELUX INTEGRA and the KLR/KLF/KUX family
<!-- doxygen-label: dev_velux_integra -->

One landing page for the VELUX side of the ecosystem: the roof-window actuators, the control pads
and hubs that make good key sources, the wired bridges, and the products controlled over 1W.

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
| KUX 110, 1W enrollment + control | ✅ Confirmed — Gear on the KLI, wait for the ready sequence, then Enroll, `low_power: false` |
| Interior blinds behind a KLI 312, 1W enrollment + control | ✅ Confirmed — `enrollment_classes: [blind, venetian_blind]`, `low_power: true` |

## Onboarding route that worked

**Key extraction, essentially always — when there's a two-way hub.** VELUX installations almost
all have a control pad or hub already, and that hub is what you extract from. Against a KLR 200
this succeeded end to end on the first attempt, including the address round.

For an installation with no hub at all — only a 1W wall switch or remote, or a device fresh out of
the box — key extraction has nothing to extract from. If a KLI remote already drives the product,
try **1W enrollment**, the route confirmed on a KUX 110 and on KLI 312 interior blinds: press the
Gear button on that remote, wait for the product's ready sequence to finish, then press the hub's
Enroll button. It gives open/close/stop without status (see
[Sending 1W commands](../configuration/oneway-transmit.md)). For 2W on such an installation, see
the SSL entry under Known quirks below.

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

- **`low_power: true` is usually required for directed commands.** VELUX actuators are frequently
  solar or battery powered and sleep between commands. Without this the hub addresses them with the
  wrong preamble. `scan_paired_devices` does not consult this property — its low-power pass is
  built specifically to reach a sleeping device and pre-fills `low_power: true` in the
  ready-to-paste snippet when it hears one — but a *directed* command (open/close/stop, status
  polls) still needs the property set to reach the device once it is registered. If a known
  device's scan result shows a `hint:` line, follow
  it: that means the registered YAML and the device's self-reported power class disagree.
- **The MSU solar awning screen ignores `stop` mid-motion.** It will not complete a 2W handshake
  while it is travelling, so there is nothing for the stop command to talk to. Open and close both
  work. Reported in issue #95; there is no 2W fix. A 1W identity could send the stop instead, but
  1W enrollment has not been retried on this screen since it was confirmed on other VELUX products
  (`low_power: true` for a solar receiver; read the enrollment classes from its remote's `0x2E`).
- **1W enrollment is a Gear press on the existing KLI, not a button on the product.** Press the
  Gear button ("open for registration") on the KLI that already drives the product for about
  1 second. Not the Pair button: that one is for a *new* control, which the hub's Enroll button
  stands in for, and Gear followed by Pair on the same switch deletes all its products. The
  product then runs a ready sequence that can take half a minute on a shutter (travel to about 10%
  closed, several jogs, back to the start); once it has finished, press Enroll on the hub. If the
  cover ends up fully closed, the closing step at the end of Enroll shows no movement, so check with
  Open afterwards. The enrollment classes must match the remote: the default fits a KLI
  310/313 (exterior shading, e.g. a KUX 110), while a KLI 312 interior blind needs
  `enrollment_classes: [blind, venetian_blind]`. The remote's own `0x2E` log lines after Gear name
  the classes. Confirmed in issue #74 (see
  [Sending 1W commands](../configuration/oneway-transmit.md)).
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

Field reports: #74 (KUX 110 and KLI 312 blind 1W enrollment), #80 (KLR 200 extraction), #87 (roll-call preamble), #95 (MSU
screen stop), #98 (INTEGRA probes).

## See also

- [Key extraction](../key-extraction.md)
- [Scan Paired Devices](../pairing.md#scan-paired-devices)
- [Troubleshooting](../troubleshooting.md#commands-are-ignored-mid-motion)
