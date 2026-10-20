# Exterior heating
<!-- doxygen-label: cfg_fan -->

> [!WARNING]
> **This platform has not been exercised against a real exterior heater.** Expect to report results.
>
> If you own an IO-Homecontrol terrace or patio heater, please try it and report what happens on the
> [issue tracker](https://github.com/laberning/home_io_control/issues): success, silence, or a wrong
> level are all useful.

Exterior heating (node type 21, `exterior_heating`) is a heater with a heat level rather than a
setpoint. It appears in Home Assistant as a fan: on or off, and a speed from 1 to 100 % that is the
heat level. Subtype 58 is on/off, so its fan has no speed. Every exterior heater is sent the main
parameter only, without a functional-parameter block, which is valid on any device.

The keys every entity platform shares — `name`, `io_device_id`, `io_device_type`, `io_subtype`,
`status_poll_interval`, `low_power`, `linked_remotes`, `home_io_control_id` — are documented in
[the configuration reference](index.md), along with the seven diagnostic companion entities.

```yaml
fan:
  - platform: home_io_control
    name: "Terrace Heater"
    io_device_id: "6F7081"
    io_device_type: "exterior_heating"

  - platform: home_io_control
    name: "Patio Heater On Off"
    io_device_id: "708192"
    io_device_type: "exterior_heating"
    io_subtype: 58
```

Scan Paired Devices and Discover & Pair print a ready-to-paste `fan:` block for an exterior heater.

## How the heat level maps to the wire

Heat `p` % is sent as the IO position `100 - p`, the same mapping a dimmable light uses. The main
parameter is the position times `0x200`, so 100 % heat is `0x0000` and off is `0xC800` (VELUX KLF 200
API specification, Table 277).

- Setting a speed sends that heat level and turns the heater on.
- Turning on without a speed uses the last level the heater was at, else full heat.
- Turning off sends 0 % heat. The fan keeps its last level, so the next turn-on returns to it.
- A subtype 58 heater is on (full heat) or off.

## What the card shows

A command shows the commanded state until the heater's status reply replaces it. The hub reads the
heater with the ordinary status request about five seconds after boot and a few seconds after every
command. A heater has no movement to wait for, so that one follow-up poll is all a command costs, and
a change made at the heater itself shows after the next command or reboot. The fan never restores a
saved state at boot; the first status poll decides what it shows.

## See also

- [Heating and climate](climate.md) — electrical heaters with a setpoint or heating levels
- [Supported devices](../supported-devices.md) — the node-type table
