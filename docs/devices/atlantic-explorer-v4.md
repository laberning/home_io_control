# Atlantic Explorer V4 water heater
<!-- doxygen-label: dev_atlantic_explorer_v4 -->

A heat-pump water heater with an electric backup element, sold by Atlantic. It speaks io-homecontrol
two-way and reports the same node type, subtype, name and brand as the
[Atlantic Calypso Connecté](atlantic-calypso.md), so the hub reads and sets the same things: water
temperature, target, mode, what is heating, energy, boost and away. Its target scale and the way it
reports boost and away differ, which is why it is declared with `io_model: explorer_v4`.
Two units are reported, one on a Heltec WiFi LoRa 32 V2 (SX1276); see
[Water heaters](../configuration/water-heater.md) for the options.

## What works

| Capability | Status |
|---|---|
| Pairing | ✅ Two units paired; both answer authenticated reads and writes. The second, a 200 L coil model, was paired from new |
| Water temperature | ✅ Read |
| Target temperature | ✅ Read: 51, 55, 57 and 59 °C match the display |
| Mode read-back (automatic / manual ECO on / manual ECO off) | ✅ Read |
| Heat pump and electric element running | ✅ Read at rest and during a boost, when both run |
| Boost state | ✅ Read from the heater's operating-mode word, the other way round from the Calypso; a boost started on the display shows in Home Assistant |
| Starting and ending a boost from Home Assistant | ✅ Boost on and off work on the heater |
| Energy since installation | ✅ Read; not compared with the display yet |
| Setting the target from Home Assistant | ✅ The display follows; see [Choosing a target](#choosing-an-explorer-v4-target) |
| Changing the mode from Home Assistant | ✅ The heater acknowledges each of the three modes and reads it back |
| Away state | ✅ Read from the target the heater reports, which drops to the bottom of its scale; the display shows the holiday |
| Starting and ending the holiday mode from Home Assistant | ✅ The card's away mode; runs until turned off; away on and off work on the heater |
| Switching the heater off | ❌ Not offered: no off state has been seen |

## Modes in Home Assistant

The display's AUTO, manual with ECO on and manual with ECO off are the card's `eco`, `heat_pump` and
`performance`. [Heat-pump water heaters](../heating.md#heat-pump-water-heaters) has the table, what
heats in each mode, and why the card shows no target in `eco`.

## Choosing an Explorer V4 target

The target scale runs from 20 to 70 °C, where the Calypso's runs to 62 °C. The heater keeps whole
degrees, so the card offers whole degrees only. In manual with ECO on it keeps at most 55 °C, and a
higher target comes back as 55 °C, on the second unit too. In manual with ECO off it kept 59, 60 and
62 °C, and the second unit kept 62 °C. The card shows the target the heater kept, read back right
after the write.

The heater answers a target it has to round with result code 10 ("parameter incoherence adjustment").
The hub sends whole degrees only, so the card never gets that answer; a target with a fraction, from an
automation for example, is refused before it is sent, and the log names it.

## Working YAML

```yaml
water_heater:
  - platform: home_io_control
    name: "Hot water"
    io_device_id: "A1B2C3"
    io_device_type: domestic_hot_water
    io_subtype: 0
    io_model: explorer_v4
    # Optional companions:
    energy:
      name: "Hot water energy"
    heat_pump_running:
      name: "Hot water heat pump"
    electric_backup_running:
      name: "Hot water electric backup"
    boost:                            # a switch
      name: "Hot water boost"
```

Discover & Pair and Scan Paired Devices print a `water_heater:` block without `io_model`, because the
heater reports nothing that tells it from a Calypso. Add `io_model: explorer_v4` by hand.

## Known quirks

- **Without `io_model: explorer_v4`** the card shows a target a few degrees below the display (46 °C
  for 51 °C, 49 °C for 55 °C), a target set from Home Assistant lands higher on the heater, the boost
  switch shows on while no boost runs, and away never shows.
- **During a boost the card shows a target of 62 °C.** That is what the heater reports while the boost
  runs; its own target is back when the boost ends.
- **While away the card shows no target**, and the heater's operating-mode word does not change. The
  hub reads away from the target the heater reports, which is at the bottom of its scale then. A target
  of 20 °C would read the same way, as away; the lowest target the heater has reported is 50 °C.
- **The boost duration write is answered with 0** when no boost runs. The boost starts all the same,
  and the heater sets the duration to 1 itself.

## What to report

If you own this heater, these answers help:

- The highest and lowest target the heater keeps in each mode.
- What the display shows while a boost started from Home Assistant runs.
- Whether the energy sensor matches the total on the display.
- The tank size, and whether another Atlantic or Thermor heat-pump water heater behaves the same.

## Evidence

Read and commanded on one Atlantic Explorer V4 with its display read beside the log: the four targets
above, the 54 °C target written as `8800`, boost started on the display and from Home Assistant, and
away from Home Assistant, each on and off
([pull request #246](https://github.com/laberning/home_io_control/pull/246)).

A second unit, an Explorer V4 200 L coil model, was paired from new with `io_model: explorer_v4` set.
It reads the water temperature and the target, takes a target from Home Assistant, and the away mode
works. A 70 °C target in `heat_pump` (manual with ECO on) came back as 55 °C, and a 62 °C target
stayed ([issue #198](https://github.com/laberning/home_io_control/issues/198)).

The model is three rows of the device readings table, selected by its capabilities
([ADR 0110](../adr/0110-a-device-values-row-says-how-it-is-written-too.md)).

## See also

- [Atlantic Calypso Connecté water heater](atlantic-calypso.md)
- [Heaters and water heaters in Home Assistant](../heating.md)
- [Water heaters](../configuration/water-heater.md)
- [Supported devices](../supported-devices.md)
