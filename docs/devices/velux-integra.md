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
| MSU 100100 solar awning screen | ⚠️ Partial — open and close work; `stop` has no effect while it moves |
| SSL solar roller shutter | ⚠️ Partial — Discover & Pair, open, close, position and status work; `stop` reaches it mid-travel, but not on every attempt |
| INTEGRA SOLAR blinds | 📣 Reported, no capture |
| SML electric roller shutter, 1W enrollment + control | ✅ Confirmed — Gear on the KLI, wait for the ready sequence, then Enroll, `low_power: false`. Two-way Discover & Pair finds nothing |
| Interior blinds behind a KLI 312, 1W enrollment + control | ✅ Confirmed — `enrollment_classes: [blind, venetian_blind]`, `low_power: true` |

## Onboarding routes

**Key extraction, essentially always — when there's a two-way hub.** VELUX installations almost
all have a control pad or hub already, and that hub is what you extract from. Against a KLR 200
this succeeded end to end on the first attempt, including the address round.

Once you hold the key, press **Scan Paired Devices** rather than pairing each device. Every device
that trusts the key answers with a ready-to-paste snippet.

For an installation with no hub at all — only a 1W wall switch or remote, or a device fresh out of
the box — key extraction has nothing to extract from. If a KLI remote already drives the product,
try **1W enrollment**, the route confirmed on an SML roller shutter and on KLI 312 interior blinds: press the
Gear button on that remote, wait for the product's ready sequence to finish, then press the hub's
Enroll button. It gives open/close/stop without status (see
[Sending 1W commands](../configuration/oneway-transmit.md)). An SSL solar roller shutter pairs
two-way directly instead; see [SSL solar roller shutter](#ssl-solar-roller-shutter) below.

## SSL solar roller shutter

The SSL pairs two-way directly with Discover & Pair. It needs no hub to extract a key from, only a
motor-side reset and a short discovery preamble.

**1. Tuning.** The motor ignores the default 1024-byte discovery preamble and answers at 32 bytes:

```yaml
home_io_control:
  tuning:
    pairing_discovery_preamble: 32              # required
    pairing_discovery_destination: "0x00003F"   # confirmed setup; not known to be required
    pairing_discovery_ack_capable: true         # confirmed setup; not known to be required
```

Pairing reuses the 32-byte preamble for every frame it sends to the motor after discovery.

**2. Reset the motor.** An SSL ships registered to its KLI wall switch. Pair it right after a reset,
which clears the motor's registrations and opens a registration window (10 minutes on the P-button
motor). The gesture depends on the motor:

- **Motor with an I/O switch and a P button** (under the top-casing cover): set the switch to **0**
  for 10 seconds, back to **I**, then hold **P** for about 10 seconds until the shutter moves.
  Repeat the gesture if the window lapses.
- **Switchless motor with a single button** ("4V" type): hold the button for about 7 seconds until
  the motor buzzes three times, wait a few seconds, then press it once briefly. The shutter moves
  to confirm.

**3. Pair.** Wait until the shutter has stopped moving, then press **Discover & Pair**. Repeat it if
the first attempt finds nothing. The motor doesn't answer the optional settings frame at the end of
pairing; the log says so, and the pairing is complete anyway. Add the printed snippet with
`low_power: true`:

```yaml
cover:
  - platform: home_io_control
    name: "Roller Shutter"
    io_device_id: "BE2142"          # from the pairing log
    io_device_type: "roller_shutter"
    io_subtype: 0
    low_power: true
```

**4. Register the wall remote again.** The reset removed it. Short-press the motor's button (P on
the I/O-switch motor; the switchless motor then moves three times, so wait until it stops). Then
press the remote's recessed registration button with a paperclip, and close the shutter fully with
the remote's down button. The remote and the hub then both work. Use a short press here: the long
hold from step 2 resets the motor again and removes the hub as well.

**In use:** open, close, set position and status work from rest. A command can take up to three
tries to reach a sleeping motor. A `stop` sent while the shutter moves reaches it, and so do the
status polls that follow, so the shutter halts and its real position arrives about a second later.
It does not land on every attempt — press stop again if the shutter keeps moving, or use the wall
remote.

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
- **A moving VELUX solar product answers only the short start preamble.** It ignores the
  1024-byte wake-up burst that reaches the same motor at rest, so the hub leads with the short
  preamble whenever it has reason to believe the motor is awake — see
  [`low_power_wake_belief`](../configuration/tuning.md#low_power_wake_belief). On the SSL solar
  roller shutter that is what lets a `stop`, and the status polls after it, land mid-travel; it
  does not land on every attempt, so press stop again if the shutter keeps moving. There is no
  such result for the MSU solar awning screen, so expect a mid-travel `stop` to do nothing there
  and use the product's own wall remote.
- **1W enrollment is a Gear press on the existing KLI, not a button on the product.** Press the
  Gear button ("open for registration") on the KLI that already drives the product for about
  1 second. Not the Pair button: that one is for a *new* control, which the hub's Enroll button
  stands in for, and Gear followed by Pair on the same switch deletes all its products. The
  product then runs a ready sequence that can take half a minute on a shutter (travel to about 10%
  closed, several jogs, back to the start); once it has finished, press Enroll on the hub. If the
  cover ends up fully closed, the closing step at the end of Enroll shows no movement, so check with
  Open afterwards. The enrollment classes must match the remote: the default fits a KLI
  310/313 (exterior shading, e.g. an SML roller shutter), while a KLI 312 interior blind needs
  `enrollment_classes: [blind, venetian_blind]`. The remote's own `0x2E` log lines after Gear name
  the classes. Confirmed in issue #74 (see
  [Sending 1W commands](../configuration/oneway-transmit.md)).
- **The KUX 110 is a power supply, not a radio bridge.** It feeds 24 V DC to a mains-powered product
  such as an SML roller shutter; the io-homecontrol receiver is in the product's own motor, and each
  KLI pairs with each product directly. An SML can equally be powered from an electric roof window
  with no KUX at all. If several products share one supply, a reset that works by interrupting
  power resets every product connected to it — including their pairings with the hub.
  Disconnect the others first if only one should be reset.
- **One Gear press can open several products at once.** Gear opens registration on every product
  that KLI drives, and each of them then registers whatever control is enrolled next. For one Home
  Assistant cover per product, enroll one identity per product and press Gear on a control that
  drives only that product.
- **VELUX uses a different 1W enrollment gesture from Somfy**, and a different priority byte on
  `CMD_EXECUTE`. Set `manufacturer: velux` on any 1W identity aimed at these devices — see
  [Sending 1W commands](../configuration/oneway-transmit.md).
## Evidence

Corpus captures across the family: 3 for the INTEGRA roof-window actuator (node `6544C6`, probe
replies, from issue #98), 1 for the successful KLR 200 key extraction (node `810BAB`, issue #80),
4 for the KIG 300 hub (node `BEFEDB`), 3 for KLR200 ↔ KUX100 bridge traffic, 3 for the KLI 310
and KLI 313 remotes, and 6 for the SSL solar roller shutter (a TaHoma pairing it; this hub pairing it,
with the long preamble ignored and the short one answered; a set-position ack; a stop and status
polls that go unanswered while it moves).

Field reports: #17 (SML discovery), #74 (SML roller shutter and KLI 312 blind 1W enrollment), #80 (KLR 200 extraction), #87 (roll-call preamble), #95 (MSU
screen stop), #98 (INTEGRA probes), #112 and #114 (SSL solar roller shutter).

## See also

- [Key extraction](../key-extraction.md)
- [Scan Paired Devices](../pairing.md#scan-paired-devices)
- [Troubleshooting](../troubleshooting.md#commands-are-ignored-mid-motion)
