# Atlantic Adelis towel dryer
<!-- doxygen-label: dev_atlantic_adelis -->

An electric towel dryer with a display, sold by Atlantic. Its display names the model
"ADELIS 2 DIGIT". A mixed model, heated by the central-heating water loop or by its electric element,
reports itself as "ADELIS 2 M". Both speak io-homecontrol two-way like the
[Atlantic AGILIA IO](atlantic-agilia-io.md) radiator, so the hub sets their temperature, mode, boost
and drying, and reads back all of them with the room temperature and whether the element heats. Two
ADELIS 2 DIGIT units are reported on an SX1276 board
([issue #202](https://github.com/laberning/home_io_control/issues/202)) and one ADELIS 2 M on an
SX1262 board (issue #202 and [issue #243](https://github.com/laberning/home_io_control/issues/243));
see [Heating and climate](../configuration/climate.md) for the options.

## Declaring it as a towel dryer

The towel dryer reports the same device type and subtype as the AGILIA radiator, an electrical
heater with an adjustable setpoint (`0x34/1`). The radio does not say which of the two a device is,
so its `climate:` entry adds `io_model: towel_dryer`. The block that Discover & Pair or Scan Paired
Devices prints names the type and subtype only, so add the line yourself.
[Choosing `io_model:`](../configuration/climate.md#choosing-io_model) says what a towel dryer does
without it, and how the hub's log helps you check the declaration: at the first poll it prints the
model name, `ADELIS 2 DIGIT` or `ADELIS 2 M`.

## What works

| Capability | Status |
|---|---|
| Discover & Pair | ✅ Reported on three units; each has a key of its own. The ADELIS 2 M paired on the first attempt on an SX1262 board |
| Target temperature | ✅ Reported working from the climate entity |
| Room temperature | ✅ Read on all three units |
| Mode read-back (off / MANUEL / PROG) | ✅ Read on the display's three modes |
| Boost and drying read-back | ✅ Read during a boost and during a drying run |
| Heating / idle | ✅ Read: the element's working rate is 100 % in boost and drying |
| Changing the mode from Home Assistant | ✅ The display follows off, MANUEL and PROG at once |
| Starting or ending boost and drying from Home Assistant | ✅ The display starts and ends both |
| Boost started on the dryer itself | ✅ Shows as the `boost` preset and `heating` at the next poll |
| Mixed ADELIS 2 M | ✅ Reported working with `io_model: towel_dryer`: mode, target, boost, drying and room temperature follow the display (issue #202). With the dryer's central-heating mode on, the element does not run and drying is not available; boost still runs. See Known quirks |
| Level the dryer applies (`heating_level:` sensor) | ⚠️ Experimental: read in a comfort period, in an eco period, in a boost and during drying (issue #202). It shows eco during a drying run, and after a change of mode or of boost it can show the old level for one or two polls |
| Central-heating mode (`central_heating_mode:` sensor) | ⚠️ Experimental: the register followed the panel's setting over four toggles on one ADELIS 2 M (issue #243). What an electric unit answers is unknown; the sensor then has no state |
| Time left and durations (`remaining_time:`, `boost_duration:`, `drying_duration:` sensors) | ⚠️ Experimental: the time left of a boost and of a drying run matched the display, and the durations set on the panel were read (issue #202). The drying duration is the one of the last run started ([Boost and drying times](../configuration/climate.md#boost-and-drying-times)) |
| Energy total (`energy:` sensor) | ⚠️ Experimental: two units answer the register, with totals that the displays were not compared with ([Energy sensor](../configuration/climate.md#energy-sensor)) |
| Target set in `auto` in an eco period (temporary override) | ✅ The dryer takes the override, shows the new temperature and follows its program again at the next change of period; `set_derogation` with `0` ends it (issue #202) |
| Target set in `auto` in a comfort period | ⚠️ Changes the comfort temperature, as the dryer's panel does: the dryer ignores an override in a comfort period (issue #202), so the hub writes the comfort setpoint after reading the dryer's level. The comfort setpoint is confirmed in `heat`; please report whether the display follows in `auto` |

## Modes in Home Assistant

The display's MANUEL, PROG and Off are the card's `heat`, `auto` and `off`; boost and drying are
presets on top. [Towel dryers](../heating.md#towel-dryers) has the table, what a target does in each
mode, and the optional sensor for the comfort and eco periods of PROG.

## Working YAML

```yaml
climate:
  - platform: home_io_control
    name: "Bathroom towel dryer"
    io_device_id: "BD684B"
    io_device_type: "electrical_heater"
    io_subtype: 1
    io_model: towel_dryer
    system_key: !secret towel_dryer_key
    heating_level:             # optional: comfort, eco or boost, as the dryer applies it
      name: "Bathroom towel dryer level"
    central_heating_mode:      # optional, experimental: on while a mixed unit heats from the water loop
      name: "Bathroom towel dryer central heating"
```

Give every towel dryer its own entry with its own `system_key:`.

## Pairing a towel dryer

The menu path is the AGILIA radiator's: on the towel dryer, open **Paramètres** → **Connectivité** →
**Connexion**, then press **Discover & Pair** on the hub. Follow
[Pairing a radiator](atlantic-agilia-io.md#pairing-a-radiator) for the key and the printed block,
and add `io_model: towel_dryer` to the block before you flash.

## Known quirks

- **Changes made on the dryer show up at the next poll.** The hub reads the dryer every five minutes
  by default, so a boost started on the dryer can take that long to appear in Home Assistant. Set
  `status_poll_interval` on the entry to read it more often.
- **A target set in `auto` is temporary in an eco period and lasting in a comfort period.** In an eco
  period it is a temporary override that holds until the next comfort period. In a comfort period it
  becomes the new comfort temperature, because the dryer ignores an override there. See
  [A target set while the heater's program runs](../heating.md#a-target-set-while-the-heaters-program-runs).
- **Frost-free and summer show as MANUEL.** On the display, lowering the target below 12 °C reaches a
  frost-free setting at 7.0 °C, and one more step reaches a summer setting at 0.0 °C. The dryer
  reports both as the MANUEL mode, so the card shows `heat` with a target of 7.0 or 0.0 °C. There is
  no fourth mode to select (issue #202).
- **The central-heating mode of an ADELIS 2 M keeps the element off.** The mixed model has a panel
  setting for heating from the water loop. While it is on, the electric element never runs, so the
  card shows `idle`, and drying does not start: the panel asks you to switch the mode off first.
  Boost still runs, because it uses a separate fan heater (issue #202). With the mode off (water
  valve closed by hand) the element runs and the card shows `heating`. The experimental `central_heating_mode:` binary
  sensor shows which mode the dryer is in
  ([Central-heating mode sensor](../configuration/climate.md#central-heating-mode-sensor)); the hub
  does not change the setting.
- **A distant unit misses replies.** One of the two ADELIS 2 DIGIT units, received at about -90 dBm,
  answered three of six roll-call attempts; the near one, at about -35 dBm, answered every attempt.

## Evidence

The ADELIS 2 M of issue #243 is reported from logs only and has no corpus capture. Corpus captures
from issue #202, on an SX1276 board:

- [A register sweep of the first unit, partly during a boost](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_register_01_sweep.yaml)
- [A register sweep of the second unit during a drying run](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_register_01_sweep_drying.yaml)
- [The mode registers read in MANUEL, PROG and off](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_mode_reads.yaml)
- [The remaining boost time read twice during a one-hour boost](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_boost_remaining.yaml)
- [Register 0x0137, the temperature in force, in a comfort period and in an eco period](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_temperature_in_force.yaml)
- [The status read in a comfort period, twice, and in an eco period](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_status_mp_fp_programme.yaml)
- [Two status reads, one with the comfort setpoint and the eco setback](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_status_mp_fp.yaml)

## See also

- [Heaters and water heaters in Home Assistant](../heating.md)
- [Heating and climate](../configuration/climate.md)
- [Atlantic AGILIA IO](atlantic-agilia-io.md)
- [Supported devices](../supported-devices.md)
