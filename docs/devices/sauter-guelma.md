# Sauter Guelma connecté water heater
<!-- doxygen-label: dev_sauter_guelma -->

An electric storage water heater sold by Sauter (Atlantic group). It speaks io-homecontrol two-way and reports
the same node type as a heat-pump water heater, so it is declared with `io_model: electric_water_heater`. One
80 L unit is reported on an SX1262 board
([pull request #248](https://github.com/laberning/home_io_control/pull/248)); see
[Electric storage water heaters](../configuration/water-heater.md#electric-storage-water-heaters) for the options.

```yaml
water_heater:
  - platform: home_io_control
    name: "Hot water"
    io_device_id: "A1B2C3"
    io_device_type: domestic_hot_water
    io_subtype: 0
    io_model: electric_water_heater
    energy:
      name: "Hot water energy"
    heating_power:
      name: "Hot water heating power"
    showers_available:
      name: "Hot water showers available"
    showers_target:
      name: "Hot water showers aimed for"
    boost:
      name: "Hot water boost"
```

## What works

| Capability | Status |
|---|---|
| Water temperature and energy since installation | ✅ Read in every state |
| Target temperature | ✅ The temperature the heater heats to, read in every state |
| Automatic and manual mode from Home Assistant | ✅ The display follows both |
| Boost from Home Assistant | ✅ Starts and ends on the display; a boost running survives a restart of the hub |
| Absence from Home Assistant | ✅ Starts and ends on the display, and the heater returns to the mode it left |
| Showers in manual mode from Home Assistant | ✅ The display follows 2, 3 and 4 showers, set as a target of 50, 60 or 70 °C |
| Showers available and aimed for | ✅ Read; they match the display |
| Heating power | ✅ Read; a separate energy meter measured the same 1000, 1500 and 2250 W levels |

Each register read and write above was checked on the heater, and the frames are in the corpus
(`sauter_guelma_exchange_register_read`, `sauter_guelma_exchange_register_write`,
`sauter_guelma_probe_status_fp_high`).

## Modes and targets

| Heater display | Home Assistant mode |
|---|---|
| AUTO | `eco` |
| Manual | `electric` |

In manual mode the heater heats to 50, 60 or 70 °C for 2, 3 or 4 showers, so the card offers those three targets
and nothing between them. A target is written as its shower count. In automatic mode and in absence the heater
chooses its own target, so the card shows none, and a target set in Home Assistant is not sent; the log says so.

The heater refuses a boost during an absence: the flag reads back off, and the switch shows it off. End the
absence first.

The heater's status reply carries nothing (its main parameter reads 0, and both its capability and
operating-mode words read `FFFF`), so the hub reads it only through its registers and sends it no status read.

## Registers

All in family `0x00`, read with `0C 60 00 <index>` and written with `0C 61 00 <index> <value>`.

| Register | Value |
|---|---|
| `0x04` | Energy since installation, Wh, 32-bit |
| `0x06` | Mode: 0 automatic, 2 manual |
| `0x17` | Water temperature, tenths of a degree |
| `0x32` | Heating power, W |
| `0x46` | Showers available |
| `0x48` | Showers of manual mode, 2 to 4 (written for a target) |
| `0x49` | Temperature it heats to, tenths of a degree (the target) |
| `0x4A` | Showers aimed for, 0 in absence |
| `0x52` | Boost, 0 or 1 (written) |
| `0x53` | Absence, 0 or 1 (written) |

The Calypso's boost and away durations (`0x00`, `0x05`) and heating state (`0x21`) answer empty here.

## See also

- [Water heaters](../configuration/water-heater.md)
- [Supported devices](../supported-devices.md)
