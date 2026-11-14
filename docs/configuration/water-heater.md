# Water heaters
<!-- doxygen-label: cfg_water_heater -->

> [!NOTE]
> The water heater platform is confirmed on one heater, an
> [Atlantic Calypso Connecté](../devices/atlantic-calypso.md): readings, target and mode. The boost
> switch and away mode are experimental. That heater takes only its own fixed targets; see
> [Choosing a target](../devices/atlantic-calypso.md#choosing-a-calypso-target).

The `water_heater:` entity drives a domestic hot water heater (node type 51, `domestic_hot_water`). It
shows the water temperature, the target temperature and the operating mode. Three optional companions
show the energy used and what is heating, and an optional switch starts and ends a boost. The card's
away mode starts and ends the heater's holiday mode. The design is recorded in
[ADR 0068](../adr/0068-a-domestic-hot-water-device-is-a-water-heater.md); boost and away follow
[ADR 0081](../adr/0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md).

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
    boost:                      # a switch (experimental)
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
- `boost` (Optional): A switch, on while a boost runs. Turning it on starts a boost and turning it off
  ends it; a boost started here asks for a duration of 1, the value the heater showed during a boost
  started on its display. Experimental: the heater has not yet been seen to follow it. The switch shows
  what the heater reports in its operating-mode word and writes nothing at boot, so a reboot does not
  end a running boost; `restore_mode` has no effect. Turning it off ends away mode too. Takes the other
  [switch](https://esphome.io/components/switch/) options.

## What the card shows

| Field | Source |
|---|---|
| Current temperature | The water temperature the heater reports |
| Target temperature | The target in the heater's status reply, 20–62 °C, step 1 °C; none in the heater's automatic mode |
| Mode | `heat_pump` (the heater's automatic mode), `eco` (manual, ECO on), `performance` (manual, ECO off) |
| Away | The heater's holiday mode, as its operating-mode word reports it (experimental) |

- Every value comes from the heater's replies; Home Assistant shows them after the first poll, about
  five seconds after boot. Each companion stays without a state until the heater has reported its
  value.
- In automatic mode (`heat_pump`) the heater chooses its own target, so the card shows none and a
  target set then is not sent; the log names the ignored target. Switch to `eco` or `performance` to
  set one.
- Setting the target sends it to the heater, which reports it back at the next status read a few
  seconds later. Setting the mode writes it, reads it back at once and reads the target right after,
  so the card shows the target the new mode holds. After either the hub polls the heater again 45
  seconds later, so what is heating follows the change. Until the first read succeeds the card shows
  the last commanded value.
- The card offers no off mode. Boost is the `boost` switch, away is the card's away mode.
- When a poll fails the hub retries with a growing delay and keeps polling at the regular interval.

## Boost and away

Boost and away are experimental: they follow the heater's operating-mode setting, and no heater has
been seen to take them yet. Writing the boost duration register alone did nothing on the one heater reported
([issue #198](https://github.com/laberning/home_io_control/issues/198)), so the hub sets the mode itself.

- **Starting** a boost writes its duration register (1) and then sets the operating mode; starting away
  writes its duration register (255, until turned off) and then sets the operating mode. The log shows
  the heater's acknowledgement of each write, so a report can say which part the heater keeps.
- **Ending** either sends the operating mode "off", which ends both. Turning boost off while away is on
  also ends away, and the other way round.
- **State** comes from the operating mode the heater reports in every poll, never from the command: the
  hub reads it right after each write and again 45 seconds later. A boost or holiday started or ended on
  the heater's display shows within one poll.
- **Away from Home Assistant** has no end date and runs until you turn it off. A holiday you set on the
  display with an end date still ends by itself, and the card follows.
- **While away** the heater holds no target of its own: the card shows none, and a target set in Home
  Assistant is not sent; the log says so.
- **A heater that does not report support** for boost or away (its capability word lacks the bit) gets
  no write for it; the log names the refusal. Until the first poll has read the capability word, a write
  is tried.

## See also

- [Atlantic Calypso Connecté](../devices/atlantic-calypso.md)
- [Supported devices](../supported-devices.md)
