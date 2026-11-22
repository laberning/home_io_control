# Sauter Hekla electric heater
<!-- doxygen-label: dev_sauter_hekla -->

An electric heater with a display and a dimmable night light, sold by Sauter, a brand of the Atlantic
group. It speaks io-homecontrol two-way like the [Atlantic AGILIA IO](atlantic-agilia-io.md) radiator,
so the hub sets its temperature and reads back the room temperature, and it adds a light entity for
the night light. Three heaters that were already paired with a Sauter Cozytouch work with a Heltec V3
(SX1262) in [issue #222](https://github.com/laberning/home_io_control/issues/222); see
[Heating and climate](../configuration/climate.md) for the options.

## What works

| Capability | Status |
|---|---|
| Discover & Pair | ✅ Confirmed on two heaters, which hand over the key of their Cozytouch installation; the third works from its address and the same key |
| Target temperature | ✅ Confirmed |
| Room temperature | ✅ Confirmed |
| `Window` sensor | ✅ Confirmed |
| The Cozytouch keeps working alongside the hub | ✅ Confirmed |
| Night light: on and off, read and switched from Home Assistant | ✅ Confirmed |
| Night light: brightness | ✅ Confirmed: the light dims with the brightness set in Home Assistant |
| Off, Heat, Auto and the presets | ⚠️ Not confirmed on a Hekla yet |
| Target set in `auto` (temporary override) | ⚠️ Experimental: no Hekla has confirmed it |
| Level the heater applies (`heating_level:` sensor) | ❌ Not available: the hub reads no level from a Hekla, so a `heating_level:` block on it is a configuration error and has to be removed |
| Occupancy detection: switch (`occupancy_detection:`) and sensor (`occupancy:`) | ✅ Confirmed: the switch turns the detection on and off, and the sensor shows presence whether the detection is on or off. With the detection on, the heater goes to its comfort temperature when it sees someone and back to eco a few minutes after it sees no one; with it off, it ignores presence |
| Energy total (`energy:` sensor) | ✅ Confirmed: the total reads correctly on three Heklas ([issue #222](https://github.com/laberning/home_io_control/issues/222)); see [Energy sensor](../configuration/climate.md#energy-sensor) |

## Modes in Home Assistant

The Hekla gets the card of a radiator with a setpoint: `off`, `heat` and `auto`, with the `Program`
and `Auto` presets. [Radiators with a setpoint](../heating.md#radiators-with-a-setpoint) has the
table and what a target does in each mode.

## Pairing a Hekla

The pairing works as on the AGILIA radiator; only the menu differs. On the heater, unlock the expert
menu and open **Menu expert** → **Paramétrages** → **Connectivité** → **Envoyer clé**, then press
**Discover & Pair** on the hub. Follow [Pairing a radiator](atlantic-agilia-io.md#pairing-a-radiator)
for the key and the printed block, and add `io_model: heater_with_light` to the block before you
flash.

- **One key for all heaters on a Cozytouch.** A heater paired with a Cozytouch holds the key of the
  Cozytouch installation, so every heater on it hands over the same key. Pair one heater; for the
  others you need only their address and the same `system_key:`. To find an address, set
  `logger: level: DEBUG` and change that heater in the Cozytouch app: the Cozytouch's frames to the
  heater show its address as `dst=` on the hub's `io_capture` log lines.
- **The Cozytouch keeps working.** Pairing with the hub reads the heater's key and leaves it
  unchanged, so the heater stays controllable from the Cozytouch app.

## Working YAML

```yaml
climate:
  - platform: home_io_control
    name: "Kids' room heater"
    io_device_id: "29FB38"
    io_device_type: "electrical_heater"
    io_subtype: 1
    io_model: heater_with_light
    system_key: !secret cozytouch_key
    night_light:
      name: "Kids' room night light"
    occupancy_detection:      # optional
      name: "Kids' room occupancy detection"
    occupancy:                # optional
      name: "Kids' room occupancy"
```

`io_model: heater_with_light` and the `night_light:` block give you the light entity;
[Heaters with a night light](../configuration/climate.md#heaters-with-a-night-light) describes how it
behaves. Without them the heater works as a radiator without a light. The two occupancy blocks are
described in [Occupancy detection](../configuration/climate.md#occupancy-detection).

## Known quirks

- **A change made in the Cozytouch app shows at the next poll**, every `status_poll_interval`, or
  shortly after the hub hears the Cozytouch command the heater. A change made on the heater's panel
  shows at the next poll.
- **A change made in Home Assistant reaches the Cozytouch app late.** The app shows it the next time
  the Cozytouch reads the heater, which can take tens of minutes; the hub cannot change that
  (issue #222).
- **The panel has four brightness levels.** It writes 100, 1000, 2000 and 10000 to the brightness
  register, which Home Assistant shows as 3 %, 33 %, 66 % and 100 %, the percentages the Cozytouch app
  shows. The app's slider snaps to the heater's levels; once it produced 3000, which Home Assistant
  shows as 70 %. Above about 3000 the light hardly gets any brighter (issue #222). Whether the heater keeps a
  value between its levels as written has not been confirmed.

## Evidence

From [issue #222](https://github.com/laberning/home_io_control/issues/222), on a Heltec V3 with the
SX1262 driver:

- [The panel's four night-light brightness levels, overheard by the hub](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/exchange/sauter_hekla_exchange_night_light_brightness_levels_sx1262.yaml)
- [The register reads of occupancy detection (`0x0101`: 0 off, 1 on) and detected presence (`0x0110`: 0 with nobody in the room, 1 while someone walks around the heater)](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/sauter_hekla_probe_occupancy_reads_sx1262.yaml)
- The reporter's confirmation of pairing, target, room temperature, the window sensor and switching
  and dimming the night light, in the issue thread.
- The reporter's confirmation of the occupancy switch and sensor in Home Assistant, including the
  sensor with the detection off and how the heater acts on presence, in the issue thread.

## See also

- [Heaters and water heaters in Home Assistant](../heating.md)
- [Heating and climate](../configuration/climate.md)
- [Atlantic AGILIA IO](atlantic-agilia-io.md)
- [Supported devices](../supported-devices.md)
