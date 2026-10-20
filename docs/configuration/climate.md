# Heating and climate
<!-- doxygen-label: cfg_climate -->

> [!WARNING]
> **This platform has not been exercised against a real radiator valve.** Expect to report results.
>
> **If you own an Atlantic / Thermor / Sauter IO-Homecontrol heater, please try it and report what
> happens** on the [issue tracker](https://github.com/laberning/home_io_control/issues) — success,
> silence, or a wrong setpoint are all useful.
>
> It is also unknown whether `set_temperature` requires a prior `power_on` or `set_mode manual`.
> This component deliberately does not synthesize either — each `heating_control` call sends
> exactly one write. Field testers: please report whether a bare `set_temperature` takes effect.

The `climate:` entity drives two kinds of electrical heater, and what its card offers follows the
heater kind. A heater with an adjustable setpoint (node type 52, subtype 1) is driven over its
private register protocol (`CMD_WRITE_PRIVATE`, 0x20): the hub reads and writes registers such as the
setpoint, the mode and the room temperature, and the entity shows what the heater reports. A plain
electrical heater (node type 52, subtype 0) is commanded with heating levels in the standard EXECUTE
and read with the ordinary status request. Reads of the register protocol are experimental: the
shape of a register read's reply is confirmed only for writes so far (see
[ADR 0054](../adr/0054-heaters-are-read-back-over-the-private-register-protocol.md)). The
heating-level control of the plain heater is experimental too, with no hardware confirmation yet (see
[ADR 0055](../adr/0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)).

Other heating devices (the heating temperature interface, exterior heating, heat pumps) have no
profile yet, so a `climate:` entity bound to one of them (by raw type value) fails at boot with a log
line saying so. Register-protocol support is unconfirmed on hardware until a test is reported in
[issue #165](https://github.com/laberning/home_io_control/issues/165). Scan Paired Devices and
Discover & Pair print a ready-to-paste `climate:` block for either kind.

## `climate:` entity

```yaml
climate:
  - platform: home_io_control
    name: "Living Room Radiator"
    io_device_id: "FEEB22"
    io_device_type: "electrical_heater"
    io_subtype: 1
    status_poll_interval: 5min  # optional; default 5 min, at least 60 s
```

What the card shows for each heater kind:

| | Electrical heater with adjustable setpoint (`io_subtype: 1`) | Plain electrical heater (`io_subtype: 0`), experimental |
|---|---|---|
| Control path | Private registers | Heating level in the standard EXECUTE |
| Modes | `off`, `heat` (manual), `auto` | `off`, `heat` (comfort level) |
| Presets | `home`, `away` (presence); custom `Program` (the heater's own schedule) | `away` (frost protection), `eco`, `comfort`, `boost`; custom `Comfort -1`, `Comfort -2`; custom `Secured`, shown when the heater reports it and never set |
| Target temperature | 7.0 °C up to the heater's own maximum (28.0 °C until it reports one), 0.5 °C step | None: the card shows a setpoint control that the entity ignores |
| Current temperature | Yes | No |
| Action | `heating`, `idle`, `off` | No |
| Companion entities | `Window` binary sensor; `Open Window Detection` switch (disabled by default) | None |

- Experimental: current temperature, setpoint, mode, action and the window state come from the
  heater's replies. Home Assistant shows them after the first poll, about five seconds after boot. A
  room-temperature sensor fault leaves the current temperature empty. `heating` means the heater's
  regulator reports heating, `idle` that it does not, and `off` that the mode is off.
- Experimental: the heater is polled every `status_poll_interval` (default 5 minutes, minimum 60 s). A
  heater with an adjustable setpoint is polled with five register reads (room temperature, setpoint,
  mode, regulator state, window sensor), and the first poll also reads its maximum setpoint and the
  open-window-detection setting, once. A plain heater is polled with one status read. When a read
  fails the hub retries with a growing delay and keeps polling at the regular interval.
- Experimental: after an accepted mode or setpoint write to a heater with an adjustable setpoint, the
  hub reads the register back, so a setpoint the heater adjusts shows as the heater holds it. After a
  level write to a plain heater, the next status read shows the level. Until the first read
  succeeds the card shows the last commanded mode, setpoint or preset.
- Experimental: a change made outside Home Assistant shows up at the next poll. The program mode of
  a heater with an adjustable setpoint appears as `auto` with the `Program` preset, and its external
  mode as `heat`. Open-window detection is read once, at the first poll, so a change made at the
  heater's panel appears after a reboot.
- The `home` / `away` presets of a heater with an adjustable setpoint write a register that cannot be
  read back, so they show the last command.

## Companion entities

The usual companion diagnostic sensors (`Device Name`, `Active Issue`, `Last Contact`, `RSSI`,
`Exchange Failures`, `Unconfirmed Exchanges`, `Last Commanded By`, `Last Command Source`) are
generated as for the other platforms. `Last Contact` shows when the heater last answered a poll.
A heater with an adjustable setpoint also gets a `Window` binary sensor (`device_class: window`) and
an `Open Window Detection` switch. Both are experimental, and they are generated only when the YAML
names the heater kind with `io_device_type: "electrical_heater"` and `io_subtype: 1`.

## Temperature range — 7.0–28.0 °C

The setpoint goes on the wire as a **16-bit little-endian value in tenths of a degree**
(`round(10 × °C)`, so 28.0 °C is `18 01`). This component accepts 7.0–28.0 °C, 28.0 being the
ceiling Atlantic radiator manuals document, and **rejects** anything outside that range rather than
clamping or truncating. Setpoints above 25.5 °C need the second byte, and that part of the encoding
is unverified on real hardware. If your radiator's own panel allows a different maximum, that is
worth reporting.

7.0–12.0 °C is the frost-protection (*hors-gel*) band on these radiators, not a comfort setpoint;
the panel's own comfort range starts at 12 °C. The codec still accepts 7–12 °C so a
frost-protection setpoint can be commanded directly.

## The `heating_control` action

Takes `device_id`, `function`, and `value`. It reaches heaters that use the private register protocol (`io_subtype: 1`); a plain heater is commanded through the `climate:` entity only. `verified` is always `false`: an acknowledged write means
the heater took the frame, and the climate entity reads the register back to show the result.

| `function` | `value` | Effect |
|---|---|---|
| `power_on` | *(ignored)* | Wake / retrieve paired devices. |
| `set_temperature` | float °C, `7.0`–`28.0` | Setpoint. |
| `set_mode` | `auto` \| `manual` \| `prog` \| `off` | Operating mode. |
| `set_presence` | `on` \| `off` | Presence / absence. |
| `set_open_window_detection` | `on` \| `off` | Switches the heater's open-window detection on or off (register `0x0106`). |
| `window_open` | *(ignored)* | Reads the heater's window sensor (register `0x010E`) and reports `window open` or `window closed` in the result `message`. |
| `midnight_sync` | *(ignored)* | Reads register `0x0130` (the comfort/eco/auto setpoint block). Despite the name, the payload is a `0x60` **read**, not a clock-set — the device's clock register is `0x010F` and this component never writes it. Provided for protocol exploration; the ACK payload is logged at DEBUG. |

```yaml
action: esphome.hioc_heltec_v2_heating_control
data:
  device_id: "FEEB22"
  function: set_temperature
  value: "20.5"
```

An unknown function, an out-of-range temperature, or a non-climate `device_id` is reported in the
result event's `message` and nothing is transmitted. A `CMD_ERROR_RESP` from the device surfaces
its decoded result code in `result_code` / `result_code_name`. For `power_on` and `midnight_sync`
(which are register reads) the `0x21` ACK payload is written to the DEBUG log — a `midnight_sync`
read returns the ~17-byte setpoint block, which can help settle the 25.5-vs-28 °C question on real
hardware.

## See also

- [Home Assistant actions](actions.md) — how to trigger an action and read its result event
- [ESPHome Climate Component](https://esphome.io/components/climate/)
