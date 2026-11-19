# Sensor-only devices
<!-- doxygen-label: cfg_sensor -->

Some devices have nothing to switch or set: they only keep values in their data objects, such as an
energy counter. They have no cover, light or climate entry to declare them, so a `sensor:` entry does it.
The entry registers the device with the hub and reads the values you list on a schedule; each value
shows in Home Assistant as a sensor of that device.

Which values a device kind offers is part of the device's support. A kind offers a reading key only
once its data has been confirmed on hardware, and the page of that device lists them. One kind offers reading
keys so far: the Atlantic heat pump (`io_device_type: heat_pump`, `io_subtype: 1`), with `heating_energy:` and
`hot_water_energy:`, its energy counters in kWh, and `outdoor_temperature:`
([Atlantic Alfea heat pump](../devices/atlantic-heat-pump.md)).
It has no status to read, so it is never sent a status read. A contributor adds a reading as
described in [Adding a device reading](../contributing.md#adding-a-device-reading).

The keys every entity platform shares (`name`, `io_device_id`, `io_device_type`, `io_subtype`,
`io_model`, `low_power`, `system_key`, `home_io_control_id`) are documented in
[the configuration reference](index.md), along with the companion diagnostic sensors every entity
gets.

```yaml
sensor:
  - platform: home_io_control
    name: "Heat pump"
    io_device_id: "ABC123"
    io_device_type: heat_pump  # a kind that offers reading keys
    io_subtype: 1
    update_interval: 1h  # optional; every reading has a default of its own, at least 5 min
    heating_energy:
      name: "Heat pump heating energy"
    hot_water_energy:
      name: "Heat pump hot water energy"
```

The entry's own sensor has no state and is not shown in Home Assistant. Its `name` names the device:
the companion diagnostic sensors are named after it, and a reading key needs a `name` of its own.

## Configuration variables:

- `io_device_id` (**Required**): The device's node ID, 6 hexadecimal characters. A device is declared
  by one entry only; a second entry on any device platform with the same `io_device_id` is a config error
  that names both entries (see [the configuration reference](index.md)).
- `io_device_type` (**Required** for a reading key): The kind of the device. A reading key is accepted
  only on a kind that has the capability the reading needs, and the error names the kind to declare.
- `update_interval` (Optional): How often the device's readings are read. The minimum is 5 minutes:
  a read opens an object, asks for it block by block and closes it, with a pause of about 650 ms
  between requests, and the device also serves other controllers. Without the key every reading is
  read at its own default.
- The reading keys of the device's kind. Each takes every
  [sensor](https://esphome.io/components/sensor/) option, and its unit, device class and state class
  are set for you.

## How a reading is read

The first read of each reading comes about a minute after the hub starts, then one every
`update_interval`. A read waits while the hub is busy with a command, while the device moves and while
another controller is using the device's data transport, and it closes the transfer at once when
another controller starts one. When the device does not answer, answers without a value, answers
an empty object or is busy, the sensor keeps its last value and the reading is read again at the next
interval. A measurement (the heat pump's outdoor temperature) shows no value after repeated misses
rather than an old one; a counter keeps its last value.
A device that answers the open with an error or with status 3, meaning it does not keep the object, is
not read again until the next boot, and the log names it once. A kind that has no status to read is sent no status read.

## See also

- [Configuration reference](index.md) — the shared keys and the companion diagnostic sensors
- [Diagnostic probes](../diagnostic-probes.md#reading-a-device-object) — reading a device's objects by hand
