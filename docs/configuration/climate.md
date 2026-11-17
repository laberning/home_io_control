# Heating and climate
<!-- doxygen-label: cfg_climate -->

The `climate:` entity drives electrical heaters, towel dryers, a heating temperature interface and a
generic heat pump. This page is about writing the entry: its keys, the `io_model:` to choose, polling,
the companion entities and the `heating_control` action. What the card then offers, and what each mode,
preset and target does on the heater, is on
[Heaters and water heaters in Home Assistant](../heating.md).

> [!NOTE]
> The platform is experimental. The [Atlantic AGILIA IO](../devices/atlantic-agilia-io.md) radiator, the
> [Atlantic Adelis](../devices/atlantic-adelis.md) towel dryer, the [Sauter Hekla](../devices/sauter-hekla.md),
> the Thermor EMOTION 3 H and a [Thermor pilot-wire interface](../devices/thermor-pilot-wire-interface.md)
> are confirmed on hardware; the heating temperature interface and the heat pump have not run on
> hardware yet. **If you own a heater, please try it and report what
> happens** on the [issue tracker](https://github.com/laberning/home_io_control/issues): success,
> silence, or a wrong setpoint are all useful.

Scan Paired Devices and Discover & Pair print a ready-to-paste `climate:` block for a heater they find.

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

## Configuration variables:

The keys every entity platform shares (`name`, `io_device_id`, `io_device_type`, `io_subtype`,
`io_model`, `low_power`, `system_key`, `linked_remotes`, `home_io_control_id`) are documented in
[the configuration reference](index.md). The options below are the heater's own.

- `io_device_type`, `io_subtype`, `io_model`: Together they name the heater kind, and the kind decides
  the card. The table in
  [What each heater kind offers](../heating.md#what-each-heater-kind-offers) lists every combination;
  the section "Choosing `io_model:`" below says when a model is needed.
- `status_poll_interval` (Optional): How often the heater is read. Default 5 minutes, minimum 60 s.
  See the section "Polling and read-back" below.
- `heating_level` (Optional): A text sensor with the level the heater applies right now. Accepted on
  `io_device_type: electrical_heater` only. See the section "Heating level sensor" below.
- `central_heating_mode` (Optional): A binary sensor, on while a mixed towel dryer runs on central
  heating. Experimental. Accepted on `io_model: towel_dryer` only. See the section "Central-heating
  mode sensor" below.
- `night_light` (Optional): A light entity for the heater's night light. Accepted on
  `io_model: heater_with_light` only. See the section "Heaters with a night light" below.
- `min_temperature`, `max_temperature` (Optional): The range a thermostat encodes its temperature
  over. Accepted on `io_device_type: heating_temperature_interface` only. See the section "Temperature
  range of a thermostat" below.
- All standard options of the ESPHome [climate](https://esphome.io/components/climate/) base schema
  also apply.

A `climate:` entity needs a heater kind that has a card. The Atlantic heat pump and boiler (node type
22, subtypes 1 and 2) have none, and exterior heating is a [fan](fan.md). An entry bound to such a
device by its raw type value fails at boot with a log line saying so.

## Choosing `io_model:`

Some products report the same type and subtype on the radio and still need different handling. The
hub cannot tell them apart, so the entry names the model.

| Your heater | Entry | Without the model |
|---|---|---|
| A radiator with a setpoint, such as an Atlantic AGILIA IO or a Thermor EMOTION 3 H | `io_subtype: 1`, no `io_model:` | |
| A towel dryer, such as an Atlantic Adelis | `io_subtype: 1`, `io_model: towel_dryer` | It runs as a radiator: the target and the room temperature work, but the mode writes go to the radiator's register, boost and drying are missing, and the window entities do nothing |
| A radiator with a night light, such as a [Sauter Hekla](../devices/sauter-hekla.md) | `io_subtype: 1`, `io_model: heater_with_light` | The heater works; a `night_light:` block is rejected |
| An interface for pilot-wire heaters, such as the [Thermor Interface Cozytouch](../devices/thermor-pilot-wire-interface.md) | `io_subtype: 0`, `io_model: pilot_wire_interface` | The card offers a `boost` preset, which the interface ignores |
| A plain electrical heater commanded by level | `io_subtype: 0`, no `io_model:` | |

The block that Discover & Pair or Scan Paired Devices prints names the type and subtype only, so add
the `io_model:` line yourself.

```yaml
climate:
  - platform: home_io_control
    name: "Bathroom towel dryer"
    io_device_id: "BD684B"
    io_device_type: "electrical_heater"
    io_subtype: 1
    io_model: towel_dryer
    system_key: !secret towel_dryer_key

  - platform: home_io_control
    name: "Bedroom heater"
    io_device_id: "A1B2C3"
    io_device_type: "electrical_heater"
    io_subtype: 0
    io_model: pilot_wire_interface
```

At the first poll the hub reads the model name of every heater with a setpoint (`io_subtype: 1`) and
logs it, for example `Device BD684B reports model name "ADELIS 2 DIGIT"`. It also logs a warning when
the declaration looks wrong: a heater declared as a towel dryer that holds no towel-dryer mode, or one
declared as anything else that does. The warning never changes what the hub does. No AGILIA radiator
has been read for the towel dryer's registers yet, so the warning says "looks like".

## Polling and read-back

How the hub reaches a heater depends on its kind:

- **A heater with a setpoint** (`io_subtype: 1`) is driven over its private register protocol
  (`CMD_WRITE_PRIVATE`, 0x20): the hub reads and writes registers such as the setpoint, the mode and
  the room temperature. Reads and writes are confirmed on an Atlantic AGILIA IO radiator; the design is
  in [ADR 0054](../adr/0054-heaters-are-read-back-over-the-private-register-protocol.md).
- **A plain electrical heater** (`io_subtype: 0`) is commanded with heating levels in the standard
  EXECUTE, the eight-byte frame a Cozytouch bridge was heard sending in issue #228, with the level in
  the main parameter. It is read with the ordinary status request. The control is confirmed on a
  Thermor pilot-wire interface (issue #228).
- **A heating temperature interface and a heat pump** carry a temperature or a named setpoint in the
  standard EXECUTE's main parameter, and report it in the ordinary status read.

Which card each kind gets is decided by its heating profile
([ADR 0055](../adr/0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)).

The heater is polled every `status_poll_interval` (default 5 minutes, minimum 60 s), and the first
poll runs about five seconds after boot. A read in the last column that gets no answer is asked again
at a later poll:

| Heater kind | One poll reads | The first poll also reads |
|---|---|---|
| Radiator with a setpoint | Five registers: room temperature, setpoint, mode, regulator state, window sensor. While its program runs, also the temperature in force. Then the open-window-detection setting, and with `io_model: heater_with_light` the light's state and brightness; a read of these that gets no answer leaves the entity as it was and is asked again at the next poll | The maximum setpoint, and for the declaration check the model name and the towel dryer's mode register |
| Towel dryer | Five registers: room temperature, setpoint, mode, temporary state, working rate. While its program runs with no boost or drying, also the temperature in force. Then one status read, which names the level the dryer applies, in every mode and whether or not a `heating_level:` sensor is configured. With a `central_heating_mode:` sensor, one more register, read before the status read | The maximum setpoint and the model name |
| Plain electrical heater, pilot-wire interface | One status read | |
| Thermostat, heat pump | One status read | |

- When a read fails the hub retries with a growing delay and keeps polling at the regular interval.
- After an accepted mode or setpoint write to a heater with a setpoint, the hub reads the register
  back, so a setpoint the heater adjusts shows as the heater holds it. It polls the heater again 45
  seconds later, so `heating` / `idle` follows the change without waiting a whole poll interval.
- After a level write to a plain heater, the next status read shows the level when the heater reports
  one.
- A target on a radiator or a towel dryer costs one to three more reads before the write, because
  the hub first asks what the heater runs; see
  [A target set while the heater's program runs](../heating.md#a-target-set-while-the-heaters-program-runs).

`Last Contact` shows when the heater last answered a poll.

## Companion entities of a heater

The usual companion diagnostic sensors (`Device Name`, `Active Issue`, `Last Contact`, `RSSI`,
`Exchange Failures`, `Unconfirmed Exchanges`, `Last Commanded By`, `Last Command Source`) are
generated as for the other platforms.

| Entity | Generated for | Notes |
|---|---|---|
| `Window` binary sensor (`device_class: window`) | `io_device_type: "electrical_heater"` with `io_subtype: 1`, without `io_model: towel_dryer` | Experimental. The heater's own window sensor |
| `Open Window Detection` switch | The same entries | Experimental, disabled by default. The setting is read on every poll, so a change made at the heater's panel or in the vendor app appears at the next poll (issue #222). A poll that misses the setting keeps the switch as it was |
| Night light | An entry with a `night_light:` block | See the section "Heaters with a night light" below |
| `Heating level` text sensor | An entry with a `heating_level:` block | See the section "Heating level sensor" below |
| Central heating mode binary sensor | An entry with a `central_heating_mode:` block | Experimental. See the section "Central-heating mode sensor" below |

A towel dryer has no window function, so it gets neither window entity. A heater declared without
`io_device_type` gets none either, because nothing in the YAML says what it is.

## Heaters with a night light

A [Sauter Hekla](../devices/sauter-hekla.md) has a dimmable night light and reports the same type and subtype as the AGILIA
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
  10000. The [Sauter Hekla](../devices/sauter-hekla.md) page lists the levels its panel offers and
  what is known about the values in between. Switching on at the brightness the heater already holds
  sends only the on command.
- Each poll reads the light's state and brightness after the heater's own registers, so a light
  switched from the Cozytouch app or at the heater shows in Home Assistant at the next poll. A
  heater that has no night light answers these reads empty; the rest of its poll still counts as
  good.
- The light changes at once: `default_transition_length` is 0 s, because each step of a fade would
  be a radio command.

## Heating level sensor

An `electrical_heater` entry takes an optional `heating_level:` text sensor that shows the level the
heater applies right now: `comfort`, `eco`, `frost protection` and so on.

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

The block takes every [text sensor](https://esphome.io/components/text_sensor/) option. The key is
accepted on `io_device_type: electrical_heater` only, and it adds no radio traffic: the level comes
from a status read the poll makes anyway. The values, and which heater kinds report a level, are in
[The level a heater applies](../heating.md#the-level-a-heater-applies).

## Central-heating mode sensor

A mixed towel dryer (an Atlantic ADELIS 2 M) heats from the house's water loop and has an electric
element as well. While its central-heating mode is on, the element never runs: the card shows
`idle`, and Boost and Drying change nothing. The optional `central_heating_mode:` binary sensor
shows that setting, so you can see why.

```yaml
climate:
  - platform: home_io_control
    name: "Bathroom towel dryer"
    io_device_id: "BD684B"
    io_device_type: "electrical_heater"
    io_subtype: 1
    io_model: towel_dryer
    central_heating_mode:
      name: "Bathroom towel dryer central heating"
```

The sensor is on in central-heating mode and off while the dryer uses its element. The block takes
every [binary sensor](https://esphome.io/components/binary_sensor/) option. The key is accepted on
`io_model: towel_dryer` only, and an entry without it reads nothing extra. It costs one register
read per poll. A dryer that answers the read without a value, as an electric unit is expected to,
leaves the sensor without a state; the hub logs that once and stops asking until the next boot.

> [!NOTE]
> Experimental. The register was read on one ADELIS 2 M while the setting was toggled on its panel
> ([issue #243](https://github.com/laberning/home_io_control/issues/243)); the hub never writes it.
> What an electric unit answers is not known. Please report your unit's result.

## Setpoint range of a radiator or towel dryer

The setpoint goes on the wire as a **16-bit little-endian value in tenths of a degree**
(`round(10 × °C)`, so 28.0 °C is `18 01`). This component accepts 7.0–28.0 °C, 28.0 being the
ceiling Atlantic radiator manuals document, and **rejects** anything outside that range rather than
clamping or truncating. Setpoints above 25.5 °C need the second byte, and that part
of the encoding is unverified on real hardware. If your radiator's own panel allows a different
maximum, that is worth reporting.

7.0–12.0 °C is the frost-protection (*hors-gel*) band on these radiators, not a comfort setpoint;
the panel's own comfort range starts at 12 °C. The codec still accepts 7–12 °C so a
frost-protection setpoint can be commanded directly.

## Temperature range of a thermostat

A heating temperature interface encodes its temperature over a range, assumed to be −40 to +80 °C.
`min_temperature:` and `max_temperature:` (each −50 to +100 °C, minimum below maximum) set another
one. They are the range the device encodes over, not the range of the card, which offers 5.0 to
30.0 °C. A heat pump uses the fixed range −40 to +80 °C, so the keys are rejected there and on every
other kind.

```yaml
climate:
  - platform: home_io_control
    name: "Thermostat"
    io_device_id: "6F7092"
    io_device_type: "heating_temperature_interface"
    min_temperature: -30   # optional: the range the thermostat encodes over
    max_temperature: 70
```

## The `heating_control` action

Takes `device_id`, `function`, and `value`. It reaches heaters that use the private register protocol
(`io_subtype: 1`); a plain heater is commanded through the `climate:` entity only. Each function
writes what it is told, whatever the heater runs. A bare `set_temperature` takes effect without a
prior `power_on` or mode write (confirmed on an AGILIA IO). `verified` is always `false`: an
acknowledged write means the heater took the frame, and the climate entity reads the register back to
show the result.

| `function` | `value` | Effect |
|---|---|---|
| `power_on` | *(ignored)* | Wake / retrieve paired devices. |
| `set_temperature` | float °C, `7.0`–`28.0` | Setpoint (the comfort temperature, register `0x0103`), written whatever the heater runs. |
| `set_derogation` | float °C, `7.0`–`28.0`, or `0` | A temporary override of the program (register `0x011D`) on a towel dryer or a radiator with a setpoint: it holds until the program's next change; `0` cancels it. Confirmed on a towel dryer in an eco period, `0` included (issue #202); a towel dryer refuses it in a comfort period and a Thermor EMOTION 3 H radiator refuses it (issue #228); experimental on other heaters. See [A target set while the heater's program runs](../heating.md#a-target-set-while-the-heaters-program-runs). Refused on heaters without a program. |
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

- [Heaters and water heaters in Home Assistant](../heating.md) — what the card offers for each heater kind
- [Home Assistant actions](actions.md) — how to trigger an action and read its result event
- [ESPHome Climate Component](https://esphome.io/components/climate/)
