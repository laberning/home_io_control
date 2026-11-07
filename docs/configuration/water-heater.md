# Water heaters
<!-- doxygen-label: cfg_water_heater -->

> [!NOTE]
> The water heater platform reads an [Atlantic Calypso Connecté](../devices/atlantic-calypso.md)
> correctly. Setting the target and the mode is **experimental**: please report whether the heater's
> display follows.

The `water_heater:` entity drives a domestic hot water heater (node type 51, `domestic_hot_water`). It
shows the water temperature, the target temperature and the operating mode, and four optional
companions show the energy used and what is heating. The design is recorded in
[ADR 0068](../adr/0068-a-domestic-hot-water-device-is-a-water-heater.md).

The keys every entity platform shares (`name`, `io_device_id`, `io_subtype`, `low_power`,
`system_key`, `linked_remotes`, `home_io_control_id`) are documented in
[the configuration reference](index.md), along with the companion diagnostic sensors every entity
gets.

```yaml
water_heater:
  - platform: home_io_control
    name: "Hot water"
    io_device_id: "F9CF0F"
    io_device_type: domestic_hot_water
    status_poll_interval: 5min  # optional; default 5 min, at least 60 s
    energy:
      name: "Hot water energy"
    heat_pump_running:
      name: "Hot water heat pump"
    electric_backup_running:
      name: "Hot water electric backup"
    boost:
      name: "Hot water boost"
```

## Configuration variables:

- `io_device_type` (**Required**): `domestic_hot_water`. The platform sends its reads and writes only
  to a kind that is read as a water heater, so the build refuses an entry without the key or with
  another kind. The kind is experimental, and the build warns about it.
- `status_poll_interval` (Optional): How often the heater is read. Default 5 minutes, minimum 60 s.
- `energy` (Optional): A sensor of the energy the heater has used since it was installed, in kWh,
  with `device_class: energy` and `state_class: total_increasing`, so it fits the energy dashboard.
  Takes every [sensor](https://esphome.io/components/sensor/) option.
- `heat_pump_running` (Optional): A binary sensor, `device_class: running`, on while the heat pump
  runs. Takes every [binary sensor](https://esphome.io/components/binary_sensor/) option.
- `electric_backup_running` (Optional): A binary sensor, `device_class: running`, on while the
  electric element heats.
- `boost` (Optional): A binary sensor, on while a boost runs.

## What the card shows

| Field | Source |
|---|---|
| Current temperature | The water temperature the heater reports |
| Target temperature | The target in the heater's status reply, 20–62 °C, step 0.5 °C |
| Mode | `heat_pump` (the heater's automatic mode), `eco` (manual, ECO on), `performance` (manual, ECO off) |

- Every value comes from the heater's replies; Home Assistant shows them after the first poll, about
  five seconds after boot. Each companion stays without a state until the heater has reported its
  value.
- Setting the target sends it to the heater, which reports it back at the next status read a few
  seconds later. Setting the mode writes it and reads it back at once. After either the hub polls the
  heater again 45 seconds later, so what is heating follows the change. Until the first read succeeds
  the card shows the last commanded value.
- The card offers no off mode, no away mode and no boost: none of them is sent yet. A boost started on
  the heater shows on the `boost` companion.
- When a poll fails the hub retries with a growing delay and keeps polling at the regular interval.

## See also

- [Atlantic Calypso Connecté](../devices/atlantic-calypso.md)
- [Supported devices](../supported-devices.md)
