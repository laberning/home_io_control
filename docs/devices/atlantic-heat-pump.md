# Atlantic Alfea heat pump
<!-- doxygen-label: dev_atlantic_heat_pump -->

An air-to-water heat pump sold by Atlantic, with heating zones and a hot-water tank, that a TaHoma
Switch or a Cozytouch reads over io-homecontrol (node type `0x16`, subtype 1). The hub reads its two
energy counters, the heating energy and the hot-water energy, in whole kilowatt-hours. It writes
nothing to the heat pump: no setpoint, no mode. One Atlantic Alfea Duo Extensa A.I. with two heating
zones is reported on an SX1276 board
([issue #244](https://github.com/laberning/home_io_control/issues/244)).

## What works

| Capability | Status |
|---|---|
| Heating energy | ✅ Read; matches the yearly total of the TaHoma app |
| Hot-water energy | ✅ Read; matches the yearly total of the TaHoma app |
| Reading alongside a TaHoma | ✅ The hub waits while the TaHoma or a room thermostat uses the heat pump's data transport |
| Setpoints, modes, outdoor temperature | ❌ Not offered: entries the TaHoma reads, readable with the `read_object` probe but not decoded as readings |

## Declaring it

The heat pump has no cover, light or climate entry, so a [`sensor:` entry](../configuration/sensor.md)
declares it. The hub needs the system key of the installation the heat pump belongs to, the one of
its TaHoma or Cozytouch.

```yaml
sensor:
  - platform: home_io_control
    name: "Heat pump"
    io_device_id: "ABC123"
    io_device_type: heat_pump
    io_subtype: 1
    heating_energy:
      name: "Heat pump heating energy"
    hot_water_energy:
      name: "Heat pump hot water energy"
```

Each counter is read once an hour unless the entry sets `update_interval` (at least 5 minutes). A
read opens the counter's entry, reads one block and closes it, about 2 seconds of radio traffic. The
heat pump is never sent a status read.

## How a counter is read

A TaHoma reads the heat pump's values as entries of one data object, each at its own offset. The hub
reads them the same way: an open of object `0x1001` in class `0x0C` at the entry's offset, which the
heat pump answers empty, then the same open of object `0x1002`, which gives the entry. The
heating energy is at offset `0x0023` and the hot-water energy at `0x0025`. The answer is a marker byte
`80` followed by the count of kWh, big-endian: `80 24 AC` is 9388 kWh. An entry that has no value
answers `81 FF FF`; the sensor then keeps its last value. The read is in the corpus as
`tests/corpus/captures/probe/atlantic_alfea_probe_read_object_heating_energy_sx1276.yaml`.

The other entries the TaHoma reads can be read with the
[`read_object` probe](../diagnostic-probes.md#reading-a-device-object) at index `30`, for example
`30:0x019B` for what reads as the outdoor temperature. Nothing in them is decoded yet.
