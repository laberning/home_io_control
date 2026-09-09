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

2W heating devices are driven with `CMD_WRITE_PRIVATE` (0x20). The `set_*` functions are
**write-only** — nothing decodes what the radiator actually did back into an entity, and the hub
only learns whether the device acknowledged the write. (`power_on` and `midnight_sync` are register
*reads*; their ACK payload is logged at DEBUG but not decoded.) State shown in Home Assistant is
"last commanded, never confirmed".

Supported device types: `heating_temperature_interface` (0x0E) is the one selectable from YAML
today. `exterior_heating` (0x15) and `heat_pump` (0x16) also map to the climate capability class
but are not yet YAML-selectable — open an issue if you have one.

## `climate:` entity

```yaml
climate:
  - platform: home_io_control
    name: "Living Room Radiator"
    io_device_id: "FEEB22"
    io_device_type: "heating_temperature_interface"
```

- Modes: `off` → mode `off`, `heat` → mode `manual`, `auto` → mode `auto`.
- Presets: `home` → presence on, `away` → presence off; the custom preset `Program` → mode `prog`.
- Target-temperature range 7.0–28.0 °C at a 0.5 °C UI step (the wire carries 0.1 °C; 0.5 is a
  UI-only choice).
- No current temperature — nothing in the protocol reports a measured room temperature.
- Entity state is published **only after a send succeeds** and is never confirmed. There is no
  status poll for climate devices.

## Companion entities

The usual companion diagnostic sensors (`Device Name`, `Active Issue`, `Last Contact`, `RSSI`,
`Exchange Failures`, `Last Commanded By`, `Last Command Source`) are generated as for the other
platforms. `Last Contact` and `Active Issue` are the only feedback a write-only device can give.

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

Takes `device_id`, `function`, and `value`. `verified` is always `false`.

| `function` | `value` | Effect |
|---|---|---|
| `power_on` | *(ignored)* | Wake / retrieve paired devices. |
| `set_temperature` | float °C, `7.0`–`28.0` | Setpoint. |
| `set_mode` | `auto` \| `manual` \| `prog` \| `off` | Operating mode. |
| `set_presence` | `on` \| `off` | Presence / absence. |
| `set_window` | `open` \| `close` | Open-window / frost protection. |
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
