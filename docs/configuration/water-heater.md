# Water heaters
<!-- doxygen-label: cfg_water_heater -->

> [!NOTE]
> The water heater platform is confirmed on two heaters, an
> [Atlantic Calypso Connecté](../devices/atlantic-calypso.md) and an
> [Atlantic Explorer V4](../devices/atlantic-explorer-v4.md): readings, target, mode, boost and away.
> An electric storage water heater, the [Sauter Guelma connecté](../devices/sauter-guelma.md), is declared
> with `io_model: electric_water_heater`; see
> [Electric storage water heaters](#electric-storage-water-heaters).
> The Calypso takes only its own fixed targets; see
> [Choosing a target](../devices/atlantic-calypso.md#choosing-a-calypso-target).

The `water_heater:` entity drives a domestic hot water heater (node type 51, `domestic_hot_water`). It
shows the water temperature, the target temperature and the operating mode. Four optional companions
show the energy used and what is heating, and an optional switch starts and ends a boost. The card's
away mode starts and ends the heater's holiday mode. Which heater mode each of the card's modes stands
for is on [Heat-pump water heaters](../heating.md#heat-pump-water-heaters). The design is recorded in
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
    heating_source:             # a text sensor
      name: "Hot water heating source"
    boost:                      # a switch
      name: "Hot water boost"
```

## Configuration variables:

- `io_device_type` (**Required**): `domestic_hot_water`. The platform sends its reads and writes only
  to a kind that is read as a water heater, so the build refuses an entry without the key or with
  another kind. The kind is experimental, and the build warns about it.
- `status_poll_interval` (Optional): How often the heater is read. Default 5 minutes, minimum 60 s.
- `io_model` (Optional): `explorer_v4` for an
  [Atlantic Explorer V4](../devices/atlantic-explorer-v4.md), with `io_subtype: 0`. It reports the same
  type, subtype, name and brand as a Calypso, so only the YAML can say which it is. The model gives the
  card the heater's 20 to 70 °C target scale, reads its boost field the other way round (2 while a
  boost runs, where a Calypso reports 1) and reads away from its target, which drops to `0000` while
  away. Without it the card shows 46 °C while the display shows 51 °C, the switch shows a boost while
  none runs, and away never shows. `electric_water_heater` for an electric storage water heater such as
  the [Sauter Guelma connecté](../devices/sauter-guelma.md), with `io_subtype: 0`; see
  [Electric storage water heaters](#electric-storage-water-heaters).
- `energy` (Optional): A sensor of the energy the heater has used since it was installed, in kWh,
  with `device_class: energy` and `state_class: total_increasing`, so it fits the energy dashboard.
  Takes every [sensor](https://esphome.io/components/sensor/) option.
- `heat_pump_running` (Optional): A binary sensor, `device_class: running`, on while the heat pump
  runs. Not on an electric storage heater, nor are `electric_backup_running` and `heating_source`. Takes every [binary sensor](https://esphome.io/components/binary_sensor/) option.
- `electric_backup_running` (Optional): A binary sensor, `device_class: running`, on while the
  electric element heats.
- `heating_source` (Optional): A text sensor that says what heats the water: `heat pump`,
  `electric backup`, `heat pump and electric backup` or `idle`. It has no state until the heater has
  reported its heating state. Takes every [text sensor](https://esphome.io/components/text_sensor/)
  option.
- `boost` (Optional): A switch, on while a boost runs. Turning it on starts a boost and turning it off
  ends it; a boost started here asks for a duration of 1, the value the heater showed during a boost
  started on its display. Confirmed on an Atlantic Calypso Connecté and an Atlantic Explorer V4. The
  switch shows what the heater reports in its operating-mode word and writes nothing at boot, so a
  reboot does not end a running boost; `restore_mode` has no effect. Turning it off ends away mode
  too. Takes the other [switch](https://esphome.io/components/switch/) options.

## What the card shows

| Field | Source |
|---|---|
| Current temperature | The water temperature the heater reports |
| Target temperature | The target in the heater's status reply, 20–62 °C on a Calypso and 20–70 °C on an Explorer V4, step 1 °C; none in the heater's automatic mode |
| Mode | The heater's operating mode, shown as `eco`, `heat_pump` or `performance`; see [Heat-pump water heaters](../heating.md#heat-pump-water-heaters) |
| Away | The heater's holiday mode, as its operating-mode word reports it (its target on an Explorer V4) |

- Every value comes from the heater's replies; Home Assistant shows them after the first poll, about
  five seconds after boot. Each companion stays without a state until the heater has reported its
  value.
- A target set while the card shows none (in `eco`, and while away) is not sent. The log says why,
  for example `Device F9CF0F: target ignored in automatic mode, the heater chooses its own` or
  `Device F9CF0F: target ignored while away, the heater holds no target then`.
- Setting the target sends it to the heater, which reports it back at the next status read a few
  seconds later. Setting the mode writes it, reads it back at once and reads the target right after,
  so the card shows the target the new mode holds. After either the hub polls the heater again 45
  seconds later, so what is heating follows the change. Until the first read succeeds the card shows
  the last commanded value.
- When a poll fails the hub retries with a growing delay and keeps polling at the regular interval.

## Boost and away

Boost and away follow the heater's operating-mode setting. Both are confirmed on an Atlantic Calypso
Connecté ([issue #198](https://github.com/laberning/home_io_control/issues/198)): boost on and off and
away on and off from Home Assistant work, and a boost started on the display shows in Home Assistant.
Writing the boost duration register alone did nothing on that heater, so the hub sets the mode itself.
An Atlantic Explorer V4 (`io_model: explorer_v4`) is confirmed the same way: boost and away from Home
Assistant, on and off, and a boost started on its display. It answers the boost duration write with 0
while no boost runs and sets the register to 1 itself once the boost starts. During a boost it reports
a target of 62 °C, which the card shows, and its own target again afterwards.

- **Starting** a boost writes its duration register (1) and then sets the operating mode; starting away
  writes its duration register (255, until turned off) and then sets the operating mode. The log shows
  the heater's acknowledgement of each write, so a report can say which part the heater keeps.
- **Ending** either sends the operating mode "off", which ends both. Turning boost off while away is on
  also ends away, and the other way round.
- **State** comes from the operating mode the heater reports in every poll, never from the command: the
  hub reads it right after each write and again 45 seconds later. An Explorer V4 keeps its operating
  mode at the same value while away, so the hub reads its away from its target instead, which is
  `0000` then. A boost or holiday started or ended on the heater's display shows within one poll.
- **Away from Home Assistant** has no end date and runs until you turn it off. A holiday you set on the
  display with an end date still ends by itself, and the card follows.
- **While away** the heater holds no target of its own: the card shows none, and a target set in Home
  Assistant is not sent; the log says so.
- **A heater that does not report support** for boost or away (its capability word lacks the bit) gets
  no write for it; the log names the refusal. Until the first poll has read the capability word, a write
  is tried.

## Electric storage water heaters

An electric storage water heater heats with its element alone and keeps its whole state in registers, so its
status reply is never read. `io_model: electric_water_heater` declares one; it is confirmed on a
[Sauter Guelma connecté](../devices/sauter-guelma.md) 80 L
([pull request #248](https://github.com/laberning/home_io_control/pull/248)).

- **Modes:** `eco` is the heater's automatic mode and `electric` its manual mode. The card offers no other.
- **Target:** the temperature the heater heats to, shown in every mode (7 °C in absence). In manual mode the
  card offers 50, 60 and 70 °C, the heater's 2, 3 and 4 showers, and a target is written as its shower count. A
  target between them is refused, and so is a target in automatic mode or in absence, where the heater chooses
  its own.
- **Boost and away** are flags the heater keeps, written directly and shown from the value read back. The heater
  refuses a boost during an absence, and the switch then shows it off.
- `energy` reads the same counter as on a heat-pump heater. Three more optional sensors take every
  [sensor](https://esphome.io/components/sensor/) option:
  - `heating_power`: the power the element draws now, in W, `device_class: power`.
  - `showers_available`: the showers of hot water the heater holds now (the steady showers of its display).
  - `showers_target`: the showers it heats for; 0 in absence.

## See also

- [Atlantic Calypso Connecté](../devices/atlantic-calypso.md)
- [Atlantic Explorer V4](../devices/atlantic-explorer-v4.md)
- [Sauter Guelma connecté](../devices/sauter-guelma.md)
- [Supported devices](../supported-devices.md)
