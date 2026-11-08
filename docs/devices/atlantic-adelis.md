# Atlantic Adelis towel dryer
<!-- doxygen-label: dev_atlantic_adelis -->

An electric towel dryer with a display, sold by Atlantic. Its display names the model
"ADELIS 2 DIGIT". It speaks io-homecontrol two-way like the
[Atlantic AGILIA IO](atlantic-agilia-io.md) radiator, so the hub sets its temperature, mode, boost
and drying, and reads back all of them with the room temperature and whether it heats. Two units are reported on an
SX1276 board ([issue #202](https://github.com/laberning/home_io_control/issues/202)); see
[Heating and climate](../configuration/climate.md#towel-dryers) for the options.

## Declaring it as a towel dryer

The towel dryer reports the same device type and subtype as the AGILIA radiator, an electrical
heater with an adjustable setpoint (`0x34/1`). The radio does not say which of the two a device is,
so its `climate:` entry adds `io_model: towel_dryer`. Without it the hub drives the towel dryer as a
radiator: the target and the room temperature still work, but the mode writes go to the radiator's
register, boost and drying are missing, and a Window sensor and an Open Window Detection switch
appear that do nothing.

The block that Discover & Pair or Scan Paired Devices prints names the type and subtype only, so add
the `io_model:` line yourself.

## What works

| Capability | Status |
|---|---|
| Discover & Pair | ✅ Reported on two units; each has a key of its own |
| Target temperature | ✅ Reported working from the climate entity |
| Room temperature | ✅ Read on both units |
| Mode read-back (off / MANUEL / PROG) | ✅ Read on the display's three modes |
| Boost and drying read-back | ✅ Read during a boost and during a drying run |
| Heating / idle | ✅ Read: the element's working rate is 100 % in boost and drying |
| Changing the mode from Home Assistant | ✅ The display follows off, MANUEL and PROG at once |
| Starting or ending boost and drying from Home Assistant | ✅ The display starts and ends both |
| Boost started on the dryer itself | ✅ Shows as the `boost` preset and `heating` at the next poll |

## Modes and presets in Home Assistant

| Towel dryer display | Home Assistant |
|---|---|
| Off | `off` |
| MANUEL — holds the set temperature | `heat` |
| PROG — follows the dryer's weekly program | `auto` |
| BOOST | the `boost` preset, on top of the mode |
| Drying | the custom `Drying` preset, on top of the mode |

Boost and drying run for the duration set on the display, and the mode underneath stays as it was.
Select the `none` preset to end a boost or a drying run from Home Assistant. Inside PROG the card
does not tell a comfort period from an eco period: the mode register reads PROG in both.

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
```

Give every towel dryer its own entry with its own `system_key:`.

## Pairing a towel dryer

The menu path is the AGILIA radiator's: on the towel dryer, open **Paramètres** → **Connectivité** →
**Connexion**, then press **Discover & Pair** on the hub. Follow
[Pairing a radiator](atlantic-agilia-io.md#pairing-a-radiator) for the key and the printed block,
and add `io_model: towel_dryer` to the block before you flash.

## Checking the declaration

At the first poll the hub reads the heater's model name and logs it once:

```text
Device BD684B reports model name "ADELIS 2 DIGIT"
```

It also logs a warning when the declaration does not fit what the heater answers: a heater declared
`io_model: towel_dryer` that holds no towel-dryer mode looks like a radiator, and a heater declared
without a model that holds one looks like a towel dryer. The warning never changes what the hub
does. An AGILIA radiator has not been read for these registers yet, so the wording stays "looks
like".

## Known quirks

- **No window function.** The towel dryer has no open-window detection, so a towel dryer entry gets
  no Window sensor and no Open Window Detection switch.
- **The regulator reads "none" during boost and drying.** The card's heating / idle comes from the
  element's working rate instead, which reads 100 % in both.
- **The remaining boost or drying time is not shown.** The dryer reports it in minutes (rounded up),
  but no entity exposes it yet.
- **Changes made on the dryer show up at the next poll.** The hub reads the dryer every five minutes
  by default, so a boost started on the dryer can take that long to appear in Home Assistant. Set
  `status_poll_interval` on the entry to read it more often.
- **The weekly program is edited on the dryer.** PROG runs the program stored on the dryer, and Home
  Assistant cannot change it. To schedule from Home Assistant instead, use `heat` and change the
  target or the mode from an automation.
- **A distant unit misses replies.** One of the two units, received at about -90 dBm, answered three
  of six roll-call attempts; the near one, at about -35 dBm, answered every attempt.

## Evidence

Corpus captures from issue #202, on an SX1276 board:

- [A register sweep of the first unit, partly during a boost](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_register_01_sweep.yaml)
- [A register sweep of the second unit during a drying run](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_register_01_sweep_drying.yaml)
- [The mode registers read in MANUEL, PROG and off](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_mode_reads.yaml)
- [The remaining boost time read twice during a one-hour boost](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_boost_remaining.yaml)
- [Two status reads, one with the comfort setpoint and the eco setback](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_adelis_probe_status_mp_fp.yaml)

## See also

- [Heating and climate](../configuration/climate.md)
- [Atlantic AGILIA IO](atlantic-agilia-io.md)
- [Supported devices](../supported-devices.md)
