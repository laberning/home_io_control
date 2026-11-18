# Sauter Guelma connecté water heater
<!-- doxygen-label: dev_sauter_guelma -->

An electric storage water heater sold by Sauter (Atlantic group). It speaks io-homecontrol two-way and reports
the same node type as a heat-pump water heater, so it is declared with `io_model: electric_water_heater`. One
80 L unit is reported on an SX1262 board; see
[Electric storage water heaters](../configuration/water-heater.md#electric-storage-water-heaters) for the options.

## What works

| Capability | Status |
|---|---|
| Water temperature and energy since installation | ✅ Read in every state |
| Target temperature | ✅ Read in every state |
| Automatic and manual mode from Home Assistant | ✅ The display follows both |
| Boost from Home Assistant | ✅ Starts and ends on the display; a boost running survives a restart of the hub |
| Absence from Home Assistant | ✅ Starts and ends on the display, and the heater returns to the mode it left |
| Showers in manual mode from Home Assistant | ✅ The display follows 2, 3 and 4 showers |
| Showers available and aimed for | ✅ Read; they match the display |
| Heating power | ✅ Read; a separate energy meter measured the same 1000, 1500 and 2250 W levels |

## Modes and targets

| Heater display | Home Assistant mode |
|---|---|
| AUTO | `eco` |
| Manual | `electric` |

In manual mode the heater heats to 50, 60 or 70 °C for 2, 3 or 4 showers, so a target set in Home Assistant takes
the nearest of the three. In automatic mode and in absence the heater chooses its own target, and one set in Home
Assistant is not sent.

The heater refuses a boost during an absence: end the absence first.

## See also

- [Water heaters](../configuration/water-heater.md)
- [Supported devices](../supported-devices.md)
