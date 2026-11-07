# Atlantic Calypso Connecté water heater
<!-- doxygen-label: dev_atlantic_calypso -->

A heat-pump water heater with an electric backup element, sold by Atlantic. It speaks io-homecontrol
two-way, so the hub reads its water temperature, target, mode, what is heating and its energy, and
sets the target and the mode. One 200 L unit is reported on an SX1276 board
([issue #198](https://github.com/laberning/home_io_control/issues/198)); see
[Water heaters](../configuration/water-heater.md) for the options.

> [!NOTE]
> Changing the target and the mode from Home Assistant is **experimental**: the hub sends both, but
> nobody has checked yet that the heater's display follows. If you own one, please report what
> happens.

## What works

| Capability | Status |
|---|---|
| Discover & Pair | ✅ Reported; the heater accepts the key push |
| Water temperature | ✅ Read in every state |
| Target temperature | ✅ Read: 54 °C and 62 °C match the display |
| Mode read-back (automatic / manual ECO on / manual ECO off) | ✅ Read; follows the display |
| Heat pump and electric element running | ✅ Read idle, with the heat pump running, and during a boost |
| Boost on / off | ✅ Read during a one-day boost and after it |
| Energy since installation | ✅ Read; matches the display's "since start" total |
| Setting the target from Home Assistant | ⚠️ Experimental |
| Changing the mode from Home Assistant | ⚠️ Experimental |
| Starting a boost or an absence from Home Assistant | ❌ Not offered yet |
| Switching the heater off | ❌ Not offered: no off state has been seen |

## Modes in Home Assistant

| Heater display | Home Assistant mode |
|---|---|
| AUTO | `heat_pump` |
| Manual, ECO on | `eco` |
| Manual, ECO off | `performance` |

The heater's manual mode heats to the target you set. In AUTO it picks its own target; the status
read showed 60 °C there. The number of showers the display offers follows the target, so it has no
entity of its own.

## Working YAML

```yaml
water_heater:
  - platform: home_io_control
    name: "Hot water"
    io_device_id: "F9CF0F"
    io_device_type: domestic_hot_water
    system_key: !secret calypso_key   # only when the heater holds a key of its own
    # Optional companions:
    energy:
      name: "Hot water energy"
    heat_pump_running:
      name: "Hot water heat pump"
    electric_backup_running:
      name: "Hot water electric backup"
    boost:
      name: "Hot water boost"
```

`io_device_type: domestic_hot_water` is required on this platform. Discover & Pair and Scan Paired
Devices print a `water_heater:` block with it. The build warns that the kind is experimental: one
heater of one brand has been read so far.

## What each value is

- **Current temperature** is the water temperature at the tank's probe, in tenths of a degree.
- **Target temperature** is what the heater reports in its status reply, between 20 and 62 °C.
- **Energy** is the energy the heater has used since it was installed, in kWh, counted up by the
  heater. It fits the Home Assistant energy dashboard.
- **Heat pump running** is on while the compressor runs, **electric backup running** while the electric
  element heats (seen during boost only), and **boost** while a boost runs.

The hub polls the heater every five minutes by default: one status read and five register reads,
each answered in about 10 ms. At the first poll it also reads the heater's brand letter and logs it
once:

```text
Device F9CF0F reports hot water brand 'A' (0x41)
```

## What to report

If you own this heater, these answers help:

- After setting the target and each mode from Home Assistant, does the display follow?
- What target does the display show in AUTO?
- What do the entities show while a holiday absence runs, after a boost ends, and with the heater
  switched off?

## Evidence

Corpus captures from issue #198, on an SX1276 board:

- [Register reads in the display's states](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/exchange/atlantic_calypso_exchange_register_read.yaml)
- [The status reads that carry the target](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_calypso_probe_status_mp_fp.yaml)

The design is recorded in
[ADR 0068](../adr/0068-a-domestic-hot-water-device-is-a-water-heater.md).

## See also

- [Water heaters](../configuration/water-heater.md)
- [Supported devices](../supported-devices.md)
