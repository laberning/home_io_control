# Heating and climate
<!-- doxygen-label: cfg_climate -->

> [!NOTE]
> **The heater with an adjustable setpoint is confirmed on the
> [Atlantic AGILIA IO](../devices/atlantic-agilia-io.md) electric radiator**: target temperature, Off /
> Heat / Auto / `Program`, room temperature and heating / idle. A bare `set_temperature` takes effect
> without a prior `power_on` or mode write. Every other heater kind on this page has not run on
> hardware yet. **If you own one, please try it and report what happens** on the
> [issue tracker](https://github.com/laberning/home_io_control/issues) — success, silence, or a wrong
> setpoint are all useful.

The `climate:` entity drives electrical heaters, a heating temperature interface and a generic heat pump,
and what its card offers follows the device kind. A towel dryer reports the same kind as a radiator,
so its entry adds `io_model: towel_dryer`. A heater with a night light adds
`io_model: heater_with_light`, and an interface for pilot-wire heaters adds
`io_model: pilot_wire_interface`; each of the three has its own section below. A heater with an
adjustable setpoint (node type 52, subtype 1) is driven over its
private register protocol (`CMD_WRITE_PRIVATE`, 0x20): the hub reads and writes registers such as the
setpoint, the mode and the room temperature, and the entity shows what the heater reports. A plain
electrical heater (node type 52, subtype 0) is commanded with heating levels in the standard EXECUTE
(the eight-byte frame a Cozytouch bridge was heard sending in issue #228, with the level in the main parameter)
and read with the ordinary status request. Reads and writes of the register protocol are confirmed
on an Atlantic AGILIA IO radiator (see
[ADR 0054](../adr/0054-heaters-are-read-back-over-the-private-register-protocol.md) for the
design). The heating-level control of the plain heater is confirmed on a Thermor pilot-wire
interface (issue #228; section "Pilot-wire interfaces" below). The interface reports no
level, so Home Assistant cannot read the level back (see
[ADR 0055](../adr/0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)).

Other heating devices (exterior heating is a [fan](fan.md); the Atlantic heat pump and boiler) have no
profile, so a `climate:` entity bound to one of them (by raw type value) fails at boot with a log
line saying so. Scan Paired Devices and Discover & Pair print a ready-to-paste `climate:` block for
either kind.

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

| | Electrical heater with adjustable setpoint (`io_subtype: 1`) | Plain electrical heater (`io_subtype: 0`); Eco, Comfort and Off confirmed, other levels untested |
|---|---|---|
| Control path | Private registers | Heating level in the standard EXECUTE |
| Modes | `off`, `heat` (manual), `auto` | `off`, `heat` (comfort level) |
| Presets | custom `Program` and `Auto` (the heater's own automatic modes) | `away` (frost protection), `eco`, `comfort`, `boost`; custom `Comfort -1`, `Comfort -2` |
| Target temperature | 7.0 °C up to the heater's own maximum (28.0 °C until it reports one), 0.5 °C step | None: the card shows a setpoint control that the entity ignores |
| Current temperature | Yes | No |
| Action | `heating`, `idle`, `off` | No |
| Companion entities | `Window` binary sensor; `Open Window Detection` switch (disabled by default) | None |

Every preset on a card sends a command when you select it. The level a heater applies right now (comfort,
eco, frost protection and so on) is a separate, optional sensor, described under "Heating level sensor"
below.

- Current temperature, setpoint, mode, action and the window state come from the heater's
  replies. Home Assistant shows them after the first poll, about five seconds after boot. A
  room-temperature sensor fault leaves the current temperature empty. `heating` means the heater's
  regulator reports heating, `idle` that it does not, and `off` that the mode is off.
- The heater is polled every `status_poll_interval` (default 5 minutes, minimum 60 s). A
  heater with an adjustable setpoint is polled with five register reads (room temperature, setpoint,
  mode, regulator state, window sensor), and the first poll also reads its maximum setpoint and the
  open-window-detection setting, once. A plain heater is polled with one status read. When a read
  fails the hub retries with a growing delay and keeps polling at the regular interval.
- After an accepted mode or setpoint write to a heater with an adjustable setpoint, the hub reads
  the register back, so a setpoint the heater adjusts shows as the heater holds it. It also polls the
  heater again 45 seconds later, so `heating` / `idle` follows the change without waiting a whole
  poll interval. After a level write to a plain heater, the next status read shows the level when
  the heater reports one. The Thermor pilot-wire interface (issue #228) reports none, so its card
  shows the mode or preset last commanded from Home Assistant, `off` after a reboot until the first
  command, and a change made at the heater or in the Cozytouch app does not show. Until the first
  read succeeds the card shows the last commanded mode, setpoint or preset.
- A change made outside Home Assistant shows up at the next poll, on a heater that reports it. A heater with an adjustable
  setpoint shows its program mode as `auto` with the `Program` preset, its AUTO mode as `auto` with
  the `Auto` preset, and its external mode as `heat`. Open-window detection is read once, at the
  first poll, so a change made at the heater's panel appears after a reboot.

## Towel dryers

An Atlantic Adelis towel dryer reports the same type and subtype as the AGILIA radiator (node type 52,
subtype 1), so the hub cannot tell the two apart on the radio. Add `io_model: towel_dryer` to its
entry to get the towel dryer's own card:

```yaml
climate:
  - platform: home_io_control
    name: "Bathroom towel dryer"
    io_device_id: "BD684B"
    io_device_type: "electrical_heater"
    io_subtype: 1
    io_model: towel_dryer
    system_key: !secret towel_dryer_key
```

| | Towel dryer (`io_model: towel_dryer`) |
|---|---|
| Control path | Private registers, with the towel dryer's own mode and temporary-state registers |
| Modes | `off`, `heat` (the display's MANUEL), `auto` (the display's PROG) |
| Presets | `boost`; custom `Drying`; `none` ends a boost or a drying run |
| Target temperature | As for the radiator: 7.0 °C up to the heater's own maximum, 0.5 °C step; while the dryer's program runs, the temperature it heats to right now |
| Current temperature | Yes |
| Action | `heating` while the element runs (including boost and drying), `idle`, `off` |
| Companion entities | None: the towel dryer has no window function |

Everything is confirmed on two Adelis units: the readings (mode, boost and drying, room temperature,
setpoint, whether the element runs) and the writes (target, mode, starting and ending boost and
drying), each checked against the dryer's display.

The towel dryer is polled with five register reads (room temperature, setpoint, mode, temporary
state, working rate) and a status read that names the level the dryer applies; the first poll also
reads its maximum setpoint. Boost and drying run for the duration set on the display.

While the mode is PROG and no boost or drying runs, each poll adds a read of the temperature the
dryer holds right now. The card then shows that temperature as the target. Setting a target from Home
Assistant writes the comfort setpoint, or, in an eco period, a temporary override that ends at the next
comfort period. In a comfort period the target follows a new setpoint at once; the dryer's own reading
replaces it at the next poll. Which period runs is the dryer's level, which the
heating level sensor shows as `comfort` or `eco`. This is experimental: it
was read on one unit, in one comfort period and one eco period; the
[Atlantic Adelis page](../devices/atlantic-adelis.md#modes-and-presets-in-home-assistant) has the detail.

Without `io_model:` a towel dryer runs as a radiator: the target and the room temperature work, but
the mode writes go to the radiator's register, boost and drying are missing, and the window entities
do nothing. At the first poll the hub reads every 52/1 heater's model name and logs it, for example
`Device BD684B reports model name "ADELIS 2 DIGIT"`. It also logs a warning when the declaration
looks wrong: a heater declared as a towel dryer that holds no towel-dryer mode, or one declared
as anything else that does. The warning never changes what the hub does.

## Pilot-wire interfaces

An interface for pilot-wire heaters, such as the Thermor Interface Cozytouch, reports the same type
and subtype as a plain electrical heater (node type 52, subtype 0), so the hub cannot tell them apart
on the radio. Add `io_model: pilot_wire_interface` to its entry:

```yaml
climate:
  - platform: home_io_control
    name: "Bedroom heater"
    io_device_id: "A1B2C3"
    io_device_type: "electrical_heater"
    io_subtype: 0
    io_model: pilot_wire_interface
```

The pilot wire carries six orders. The card offers the modes `off` and `heat` and the presets `away`
(frost protection), `eco`, `comfort`, `Comfort -1` and `Comfort -2`. All five presets and `off` are
confirmed on the heater's display (issue #228). It has no boost: the interface ignores one, so the card
does not show it. The sixth order, `secured`, is one a heater reports and a controller cannot set, so
it is no preset; an interface that reports it shows `heat` on the card and `secured` on the
heating level sensor. The commands are the same heating-level EXECUTE as for
a plain heater. A plain heater without `io_model:` keeps its `boost` preset.

## Heaters with a night light

A Sauter Hekla has a dimmable night light and reports the same type and subtype as the AGILIA
radiator. Add `io_model: heater_with_light` to its entry, and a `night_light:` block to get a light
entity for it:

```yaml
climate:
  - platform: home_io_control
    name: "Kids' room heater"
    io_device_id: "29FB38"
    io_device_type: "electrical_heater"
    io_subtype: 1
    io_model: heater_with_light
    night_light:
      name: "Kids' room night light"
```

The heater itself works as the radiator does: the same card, the same window entities. `night_light:`
takes the options of any ESPHome [light](https://esphome.io/components/light/), and it is rejected on a
heater without `io_model: heater_with_light`.

> [!NOTE]
> **The night light is experimental.** Reading it and switching it on and off from Home Assistant are
> confirmed on a Hekla (issue #222). Dimming from Home Assistant has not been confirmed on a heater yet. If you try it, please
> [report](https://github.com/laberning/home_io_control/issues) what the light does.

- The light shows what the heater reports. It has no state until the first poll, and every switch or
  brightness change the heater acknowledges is read back. A change it does not acknowledge leaves the
  light showing the heater's last reading.
- Brightness is a percentage, 100 % in Home Assistant being the heater's full brightness. The heater's
  brightness register runs from 0 to 10000 and the light scales it linearly: 1 % is 100 and 100 % is
  10000. A Hekla's panel offers four levels, which show as 1 %, 10 %, 20 % and 100 %. Whether the
  heater accepts the values in between has not been confirmed. Switching on at the brightness the
  heater already holds sends only the on command.
- Each poll reads the light's state and brightness after the heater's own registers, so a light
  switched from the Cozytouch app or at the heater shows in Home Assistant at the next poll. A
  heater that has no night light answers these reads empty; the rest of its poll still counts as
  good.
- The light changes at once: `default_transition_length` is 0 s, because each step of a fade would
  be a radio command.
- A Hekla shares its node type with the Adelis towel dryer, so its weekly program may report a level
  the way the dryer does, but the hub does not read it from a heater with an adjustable setpoint yet:
  the heating level sensor stays without a state. If you run one in its
  program, please [open an issue](https://github.com/laberning/home_io_control/issues) so we can check it.

## Heating level sensor

An `electrical_heater` entry takes an optional `heating_level:` text sensor that shows the level the
heater applies right now. It is separate from the card's preset, which only offers what you can select.

```yaml
climate:
  - platform: home_io_control
    name: "Bathroom towel dryer"
    io_device_id: "BD684B"
    io_device_type: "electrical_heater"
    io_subtype: 1
    io_model: towel_dryer
    heating_level:
      name: "Bathroom towel dryer level"
```

- The value is one of `off`, `frost protection`, `eco`, `comfort -2`, `comfort -1`, `comfort`,
  `boost` or `secured`. The sensor has no state until the heater has reported a level. It takes every
  [text sensor](https://esphome.io/components/text_sensor/) option.
- The level comes from the heater's status read. A plain heater is polled with it. A towel dryer gets
  one such read at the end of every poll, in every mode, whether or not the sensor is configured, which
  costs one more exchange per poll. In PROG it shows `comfort` or `eco` as the program changes period;
  with a boost on top it shows `boost`.
- The sensor stays without a state where the heater reports no level: a Thermor pilot-wire interface
  (issue #228), and an AGILIA radiator or a Hekla, whose level the hub does not read yet. A level that
  is not in the list leaves the last value shown.
- The key is accepted on `io_device_type: electrical_heater` only. It is experimental on a towel
  dryer: the status read is captured on one unit in comfort, eco and boost, not yet during a drying
  run or a temporary override. If you try it, please
  [report](https://github.com/laberning/home_io_control/issues) what the sensor shows.

## Heating temperature interface and heat pump

Experimental and untested: no capture or hardware report exists for either. A heating temperature
interface (node type 14, `io_device_type: heating_temperature_interface`) and a generic heat pump
(node type 22, subtype 0, `io_device_type: heat_pump`) carry a temperature or a named setpoint in the
standard EXECUTE's main parameter, and report it in the ordinary status read. The Atlantic heat pump
and boiler subtypes (22/1, 22/2) have a different protocol and are not supported.

| | Heating temperature interface | Heat pump |
|---|---|---|
| Modes | `off`, `heat` (manual), `auto` | `off`, `heat` |
| Presets | `comfort`, `eco`; custom `Secured` | `comfort`, `eco`; custom `Setback`, `Halted` |
| Target temperature | 5.0–30.0 °C, 0.5 °C step | 5.0–30.0 °C, 0.5 °C step |
| Current temperature, action | None: nothing in the protocol this entity reads carries one | None |
| Temperature range on the wire | −40 to +80 °C, assumed; `min_temperature:` / `max_temperature:` override it | −40 to +80 °C, fixed |
| Companion entities | None | None |

- A setpoint is sent as a temperature in the main parameter. A thermostat's setpoint also carries the
  manual mode; a heat pump's setpoint carries nothing else.
- `comfort`, `eco` and the other presets select the device's own named setpoints. This entity does not
  write those setpoints.
- On a thermostat, `heat` sends the current target temperature (it needs one: set a target first, or
  wait for the first status read), `auto` selects the comfort setpoint in auto mode, and `off` switches
  it off. On a heat pump, `heat` selects the comfort setpoint with the on state and `off` switches it
  off. There is no cooling.
- The card shows the setpoint or preset from the device's status read, a few seconds after each command.
  The mode is the last command, because nothing reads it back; before any command a device that reports
  a setpoint is shown as heating.
- `min_temperature:` and `max_temperature:` (each −50 to +100 °C, minimum below maximum) are the range a
  thermostat encodes its temperature over, not the range of the card. They are accepted on a
  `heating_temperature_interface` only.

```yaml
climate:
  - platform: home_io_control
    name: "Thermostat"
    io_device_id: "6F7092"
    io_device_type: "heating_temperature_interface"
    min_temperature: -30   # optional: the range the thermostat encodes over
    max_temperature: 70
```

## Companion entities

The usual companion diagnostic sensors (`Device Name`, `Active Issue`, `Last Contact`, `RSSI`,
`Exchange Failures`, `Unconfirmed Exchanges`, `Last Commanded By`, `Last Command Source`) are
generated as for the other platforms. `Last Contact` shows when the heater last answered a poll.
A heater with an adjustable setpoint also gets a `Window` binary sensor (`device_class: window`) and
an `Open Window Detection` switch. Both are experimental, and they are generated only when the YAML
names the heater kind with `io_device_type: "electrical_heater"` and `io_subtype: 1`, and not for a
towel dryer (`io_model: towel_dryer`). A heater with a night light gets a light entity when its entry
has a `night_light:` block (section "Heaters with a night light" above), and an electrical heater gets
a `Heating level` text sensor when its entry has a `heating_level:` block
(section "Heating level sensor" above).

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
| `set_derogation` | float °C, `7.0`–`28.0`, or `0` | Towel dryer in its program: a temporary override of the eco temperature that ends at the next comfort period (register `0x011D`); `0` cancels it. Experimental, not yet confirmed on a dryer. Refused on heaters without program periods. |
| `set_mode` | `auto` \| `manual` \| `prog` \| `off` | Operating mode. |
| `set_presence` | `on` \| `off` | Presence / absence. The AGILIA IO acknowledges it; no visible effect observed. |
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
