# Heaters and water heaters in Home Assistant
<!-- doxygen-label: heating -->

This page says what the Home Assistant card of each heater offers and what each choice does on the
heater. To write the YAML entry, see [Heating and climate](configuration/climate.md) and
[Water heaters](configuration/water-heater.md). For one product's pairing, quirks and evidence, see its
page under [Supported devices](supported-devices.md).

Read the rules once, then find your heater in the table "What each heater kind offers" and read the
section of its kind. Two things that several kinds share have sections of their own after the kinds: a
target set while a program runs, and the level a heater applies.

## How a heater's modes become Home Assistant modes

A heater's display names its modes in the vendor's words: BASIC, MANUEL, PROG, AUTO, Confort, Eco.
Home Assistant has a fixed list of modes for a climate card, and an integration may not add one
([climate entity](https://developers.home-assistant.io/docs/core/entity/climate/)). Each heater mode
is therefore shown as the Home Assistant mode that means the same thing.

### The mode says who decides the temperature

| Home Assistant mode | Meaning | Heater modes shown this way |
|---|---|---|
| `off` | The heater does not heat. | Off, and the pilot wire's stop order |
| `heat` | You decide: the heater holds the temperature or the level you set. | BASIC, MANUEL, the levels of a level heater |
| `auto` | The heater follows its own weekly program or what it has learned. | PROG, AUTO |

A heater with two automatic modes shows both as `auto`, and a preset tells them apart.

### A preset is a choice you make

A preset is a variant of a mode that you select on the card. Home Assistant has standard presets
(`away`, `eco`, `comfort`, `boost`); where none fits, the card adds one under the heater's own name,
such as `Program`, `Drying` or `Comfort -1`.

Every preset on a card sends a command when you select it. The card never lists a preset that only
reports something. What the heater applies right now, such as the comfort or the eco period of its
program, is a separate sensor (section "The level a heater applies" below).

### The target follows the mode

- On a radiator or a towel dryer in `heat`, the target is the comfort temperature. Setting it changes
  that temperature for good.
- On a radiator or a towel dryer in `auto`, the target is a change for now. It holds until the program
  changes next, and the program keeps its own temperatures (section "A target set while the heater's
  program runs" below).
- A heater that is commanded by level (comfort, eco, frost protection) has no target. Its card still
  draws a temperature control, and the entity ignores it.
- A thermostat and a heat pump take the target as a setpoint in any mode (section "Thermostats and heat
  pumps" below).

### What the card shows comes from the heater

- **A heater that reports its state** shows what it reports: mode, preset, target, room temperature and
  whether it heats. The values appear after the first poll, about five seconds after the board starts.
  Until that first read succeeds, the card shows the mode, target or preset you last sent.
- **A change made outside Home Assistant**, on the heater's panel or in the vendor's app, shows at the
  next poll. The poll interval is the entry's
  [`status_poll_interval`](configuration/climate.md#polling-and-read-back).
- **`heating`, `idle` and `off`** under the mode are the heater's own report: `heating` while its
  regulator or element runs, `idle` while it does not, `off` while the mode is off. A room-temperature
  sensor fault leaves the room temperature empty.
- **A heater that reports nothing** shows the mode or preset last sent from Home Assistant, and `off`
  after a restart until the first command. A change made at the heater or in the vendor's app does
  not show. The [Thermor pilot-wire interface](devices/thermor-pilot-wire-interface.md) is such a
  heater (issue #228).

### A water heater has a fixed list of modes

Home Assistant gives a water heater a fixed list of modes and no presets
([water heater entity](https://developers.home-assistant.io/docs/core/entity/water-heater/)). Each of
the heater's modes is shown as the Home Assistant mode that describes what heats the water, not as
the name on the heater's display
([ADR 0090](adr/0090-a-water-heaters-modes-are-chosen-by-what-heats.md)).

## What each heater kind offers

Which kind your product is:

| Product | Heater kind |
|---|---|
| [Atlantic AGILIA IO](devices/atlantic-agilia-io.md), Thermor EMOTION 3 H | Radiator with a setpoint |
| [Sauter Hekla](devices/sauter-hekla.md) | Radiator with a setpoint and a night light |
| [Atlantic Adelis](devices/atlantic-adelis.md) | Towel dryer |
| [Thermor Interface Cozytouch for pilot-wire heaters](devices/thermor-pilot-wire-interface.md) | Pilot-wire interface |
| [Atlantic Calypso Connecté](devices/atlantic-calypso.md) | Heat-pump water heater |

The card of each kind, keyed by the three keys of your entry. The section of the kind has the detail.

| Heater kind | `io_device_type` | `io_subtype` | `io_model` | Modes | Presets | Target | Room temperature | Heating / idle |
|---|---|---|---|---|---|---|---|---|
| Radiator with a setpoint (also with `io_model: heater_with_light`) | `electrical_heater` | `1` | | `off`, `heat`, `auto` | `Program`, `Auto` | yes | yes | yes |
| Towel dryer | `electrical_heater` | `1` | `towel_dryer` | `off`, `heat`, `auto` | `none`, `boost`, `Drying` | yes | yes | yes |
| Level heater | `electrical_heater` | `0` | | `off`, `heat` | `away`, `eco`, `comfort`, `boost`, `Comfort -1`, `Comfort -2` | no | no | no |
| Pilot-wire interface | `electrical_heater` | `0` | `pilot_wire_interface` | `off`, `heat` | `away`, `eco`, `comfort`, `Comfort -1`, `Comfort -2` | no | no | no |
| Thermostat | `heating_temperature_interface` | `0` | | `off`, `heat`, `auto` | `comfort`, `eco`, `Secured` | yes | no | no |
| Heat pump | `heat_pump` | `0` | | `off`, `heat` | `comfort`, `eco`, `Setback`, `Halted` | yes | no | no |

A water heater is a `water_heater:` entity with its own card; the section "Heat-pump water heaters"
below describes it.

## Radiators with a setpoint

An electric radiator that holds a temperature you set and has a weekly program of its own: the
[Atlantic AGILIA IO](devices/atlantic-agilia-io.md), the [Sauter Hekla](devices/sauter-hekla.md) and
the Thermor EMOTION 3 H.
The display words below are the AGILIA IO's, from its
[installation manual](https://www.atlanticheat.co.uk/assets/uploads/general/Agilia_IO_Digital_Installation_Manual_no-lines.pdf).

| Radiator display | Mode | Preset | What a target sets |
|---|---|---|---|
| BASIC: holds the set temperature, smart functions off | `heat` | | The comfort temperature |
| PROG: follows the radiator's weekly program | `auto` | `Program` | A temporary override (experimental; the EMOTION 3 H refuses it) |
| AUTO: learns when the room is used and adjusts the temperature | `auto` | `Auto` | A temporary override (experimental; the EMOTION 3 H refuses it) |
| Off | `off` | | The comfort temperature |

- Select `Program` or `Auto` to switch between the two automatic modes. Selecting `auto` on the card
  starts AUTO. `heat` or `off` leaves both.
- A radiator whose setpoint comes from another controller reports a mode of its own, which the card
  shows as `heat`. Home Assistant cannot select it.
- The target runs from 7.0 °C to the radiator's own maximum (28.0 °C until it reports one) in steps
  of 0.5 °C. The [setpoint range](configuration/climate.md#setpoint-range-of-a-radiator-or-towel-dryer)
  explains the band below 12 °C.
- `heating` and `idle` come from the radiator's regulator.
- The entry also gets a `Window` sensor and an `Open Window Detection` switch, and a Sauter Hekla a
  night light; see [Companion entities](configuration/climate.md#companion-entities-of-a-heater). An
  optional switch and sensor show the heater's
  [occupancy detection](configuration/climate.md#occupancy-detection) (experimental), and an optional
  [energy sensor](configuration/climate.md#energy-sensor) shows the heater's energy total (confirmed on a
  Sauter Tahual).

Target, the three modes, both presets, the room temperature and `heating` / `idle` are confirmed on
three AGILIA IO radiators, each read back (issue #165). The target and the room temperature are
confirmed on a Sauter Hekla (issue #222) and the target on a Thermor EMOTION 3 H (issue #228). On the
EMOTION 3 H the temperature in force reads correctly (register `0x012A`, in `auto` it follows the
comfort temperature), but the radiator refuses the temporary override in AUTO and in PROG: the
write is acknowledged and the register reads back 0. The card keeps the value in force and the log
has a warning. Until a radiator takes the override, change its comfort temperature in `heat` and
switch back to `auto`.
The override is experimental on the AGILIA IO and the Sauter Hekla: no radiator has been read for the
two registers it uses, and a radiator without them keeps writing the comfort temperature in every
mode. If you run one of these in `auto`, please
[open an issue](https://github.com/laberning/home_io_control/issues) with what the card does after a
target.

## Towel dryers

An electric towel dryer with a display, declared with `io_model: towel_dryer`: the
[Atlantic Adelis](devices/atlantic-adelis.md). Boost and drying are timed runs on top of the mode,
so they are presets, and the mode underneath stays as it was.

| Towel dryer display | Mode | Preset | What a target sets |
|---|---|---|---|
| MANUEL: holds the set temperature | `heat` | | The comfort temperature |
| PROG: follows the dryer's weekly program | `auto` | | In a comfort period the comfort temperature, in an eco period a temporary override |
| Off | `off` | | The comfort temperature |
| BOOST | the mode underneath | `boost` | The comfort temperature |
| Drying | the mode underneath | `Drying` | The comfort temperature |

- Select `boost` or `Drying` to start a run, and `none` to end the one that runs. Both run for the
  duration set on the dryer's display.
- A boost started on the dryer itself shows as the `boost` preset and `heating` at the next poll.
- The target runs from 7.0 °C to the dryer's own maximum (28.0 °C until it reports one) in steps of
  0.5 °C. While PROG runs with no boost or drying on top, the card shows the temperature the dryer
  heats to right now as the target.
- `heating` and `idle` come from the element's working rate, which reads 100 % during boost and
  drying. The dryer's regulator reads "none" in both.
- The comfort and eco periods of PROG are not presets, because nothing selects them. The
  `heating_level:` sensor shows them (section "The level a heater applies" below).
- A towel dryer has no window function, so its entry gets no window entities.

Mode, boost and drying, target, room temperature and `heating` / `idle` are confirmed on two electric
Adelis units, the writes each checked against the dryer's display (issue #202), and a third, mixed
unit follows the display for mode, target, boost and drying (issue #243). The mixed model has a
central-heating mode that keeps the element off: the card then shows `idle`, and drying does not
start, while boost still runs on the dryer's separate fan heater. A target in `auto` changes the
comfort temperature in a comfort period and is a temporary override in an eco period (see "A
target set while the heater's program runs" below). The level sensor is experimental.

## Level heaters and pilot-wire interfaces

A plain electrical heater takes no temperature from the hub. It is told which level to apply. A
pilot-wire interface passes the same levels on to a heater over its pilot wire, as the six orders
that wire carries.

| Level or pilot-wire order | Mode | Preset |
|---|---|---|
| Comfort | `heat` | `comfort` |
| Comfort -1: one degree below comfort | `heat` | `Comfort -1` |
| Comfort -2: two degrees below comfort | `heat` | `Comfort -2` |
| Eco | `heat` | `eco` |
| Frost protection | `heat` | `away` |
| Off, the pilot wire's stop order | `off` | |
| Boost | `heat` | `boost`, on a level heater without `io_model:` only |
| Secured | `heat` | none: a heater reports it, a controller cannot set it |

- Selecting `heat` sends the comfort level. Each preset sends its level.
- The card has no target, no room temperature and no `heating` / `idle`.
- **A pilot-wire interface** (`io_model: pilot_wire_interface`) has no boost: the interface ignores
  one, so its card does not offer it. All five presets and `off` are confirmed on the heater's
  display behind a [Thermor Interface Cozytouch](devices/thermor-pilot-wire-interface.md)
  (issue #228), whose page lists what the display shows for each. The interface reports no level, so
  the card shows what Home Assistant last sent.
- **A level heater without `io_model:`** keeps the `boost` preset. Boost is untested, and no heater
  that reports its level back has been seen. One that reports `secured` shows `heat` with no preset
  on the card and `secured` on the `heating_level:` sensor.

## Thermostats and heat pumps

*Experimental and untested: no capture or hardware report exists for either.*

A heating temperature interface (`io_device_type: heating_temperature_interface`, node type 14) and
a generic heat pump (`io_device_type: heat_pump`, node type 22, subtype 0) take a temperature or one
of the device's own named setpoints.

| | Thermostat | Heat pump |
|---|---|---|
| `off` | Switches it off | Switches it off |
| `heat` | Sends the card's target as a manual setpoint; it needs one, so set a target first or wait for the first status read | Selects the comfort setpoint and switches it on |
| `auto` | Selects the comfort setpoint in the device's automatic mode | Not offered |
| Presets | `comfort`, `eco`, `Secured`: the device's own named setpoints | `comfort`, `eco`, `Setback`, `Halted`: the device's own named setpoints |
| Target | 5.0 to 30.0 °C in steps of 0.5 °C; carries the manual mode with it | 5.0 to 30.0 °C in steps of 0.5 °C; carries nothing else |

- A preset selects a named setpoint. The entity does not change the temperature stored behind it.
- The card shows the setpoint or preset from the device's status read, a few seconds after each
  command. The mode is the last command, because nothing reads it back. Before any command, a device
  that reports a setpoint is shown as heating.
- Neither card has a room temperature or `heating` / `idle`: nothing the entity reads carries one.
- There is no cooling.
- The Atlantic heat pump and boiler subtypes (22/1, 22/2) use a different protocol and have no card.
  The Atlantic heat pump's energy counters and outdoor temperature are read through a `sensor:` entry instead, see
  [Atlantic Alfea heat pump](devices/atlantic-heat-pump.md); the boiler is not supported.

## Heat-pump water heaters

A water heater with a heat pump and an electric backup element: the
[Atlantic Calypso Connecté](devices/atlantic-calypso.md). It is a `water_heater:` entity. Each mode
is the Home Assistant mode that describes what heats.

| Heater display | Home Assistant mode | What heats | Target |
|---|---|---|---|
| AUTO | `eco` | The heater adapts to your consumption; the electric element helps when needed | None: the heater chooses its own, between 50 and 62 °C |
| Manual, ECO on | `heat_pump` | The heat pump only | The one you set, up to 50 or 54 °C |
| Manual, ECO off | `performance` | The heat pump, with the electric element assisting | The one you set, up to 62 °C |

- Each mode follows what Home Assistant says it means: `eco` saves energy, `heat_pump` heats slowest
  and uses the least energy, `performance` heats hardest.
- In `eco` the heater's display shows no target, so the card shows none. A target set then is not
  sent, and the log names it. Select `heat_pump` or `performance` first; the hub reads the target
  again right after a mode change, so the card shows the heater's manual target within a second or
  two.
- The card has no `off` mode: no water heater has been seen reporting an off state.
- A boost is the optional `boost` switch, and the heater's holiday mode is the card's away mode; see
  [Boost and away](configuration/water-heater.md#boost-and-away).
- The optional `heating_source` sensor says what heats right now: `heat pump`, `electric backup`,
  `heat pump and electric backup` or `idle`.

All three modes, the target, boost and away are confirmed on one Calypso (issue #198). That manual
ECO on heats with the heat pump alone comes from the heater's manual and has not been confirmed by
watching the element; the `heating_source` sensor shows what runs, so you can check it.

## A target set while the heater's program runs

*Confirmed on a towel dryer (issue #202). A Thermor EMOTION 3 H radiator refuses the override altogether (issue #228). Experimental on other heaters.*

A heater with its own weekly program shows `auto` while that program runs: PROG on a towel dryer,
PROG and AUTO on a radiator with a setpoint. A target you set then is a temporary override of the
program. *Dérogation* is the French word for such an exception, which is where the action's
`set_derogation` function gets its name.

- **While the program runs** a target is written to the heater's override register (`0x011D`) and
  read back. If the heater takes it, it holds until the program's next change. The program's comfort
  temperature is not touched. A towel dryer does this in an eco period only; see the next item.
- **A towel dryer in a comfort period** ignores the override, as its own panel changes the comfort
  temperature there. So a target you set while the dryer's program is in a comfort period is written
  as the new comfort temperature, and it stays until you change it again. The same gesture is
  temporary in an eco period and lasting in a comfort period. The hub asks the dryer for its level
  just before the write to know which period it is in. During a boost or a drying run the target is
  the comfort temperature too.
- **In `heat`** a target is the comfort temperature, as on the heater's own display. So is a target
  set while a towel dryer runs a boost or a drying run on top of its program.
- **The card's target in `auto`** is the temperature the heater holds right now, read at every poll
  (register `0x0137` on a towel dryer, `0x012A` on a radiator). When you switch the card to `auto`,
  the hub reads it right after the heater confirms the mode, so the card shows it straight away. If
  the heater does not answer that read, the next poll shows it.
- **A target first asks the heater what it runs.** Which register a target belongs in depends on what
  the heater runs at that moment, and the heater can change that on its own: you switch it on its
  panel, or a boost or drying run ends on its timer. So before it writes, the hub reads the heater's
  mode and, on a towel dryer in its program, whether a boost or drying run is on top. While the
  program runs and the heater has not reported a temperature in force since the restart, the hub
  reads that too. A towel dryer in its program is then asked for its level, which tells a comfort
  from an eco period. The card shows what these reads found. Presets and mode changes are sent
  without a read.
- **If the heater does not answer these reads**, or a towel dryer in its program gives no comfort or
  eco level, the hub changes nothing and logs a warning with the device id. The target in Home
  Assistant stays the previous one, although an open card can show your value until you open it
  again. Set the target again when the heater is in range. The hub never falls back to the comfort
  temperature here, because that would change the program for good.
- **A heater that does not hold the register** answers the temperature-in-force read without a
  value. The hub then stops asking until the next restart, the card's target stays the comfort
  temperature, and a target writes the comfort temperature. A heater that has reported a temperature
  in force once keeps the override rule until the next restart, even when a later read of that
  register gets no reply or no value.
- **A heater that refuses the override** is logged as a warning with the device id, the value asked
  for and what the register holds. Nothing else is written, and the card shows the heater's own
  target again at once. A Thermor EMOTION 3 H radiator does this in every period: the write is
  acknowledged and the register reads back 0. A target in `auto` does nothing yet on that radiator;
  change the comfort temperature in `heat` instead, as described below.
- **To end an override early**, call the
  [`heating_control` action](configuration/climate.md#the-heating_control-action) with
  `set_derogation` and `0`. An override also ends by itself, as the heater decides. Home Assistant
  sends no call when you select the mode the card already shows, so selecting `auto` again cannot
  end it.

To change a program's comfort temperature, select `heat`, set the target there and select `auto`
again, or change it on the heater. The weekly program itself is edited on the heater; Home Assistant
cannot change it. To schedule from Home Assistant instead, use `heat` and change the target or the
mode from an automation.

The rule and the reasons for it are recorded in
[ADR 0092](adr/0092-a-target-set-in-auto-is-a-temporary-override.md).

## The level a heater applies

A heater works at a level: `off`, `frost protection`, `eco`, `comfort -2`, `comfort -1`, `comfort`,
`boost` or `secured`. The optional
[`heating_level:` sensor](configuration/climate.md#heating-level-sensor) shows the level the heater
reports. Use it to see, or to react in an automation to, the comfort and eco periods of a program.

| Heater kind | What the sensor shows |
|---|---|
| Towel dryer | The level in every mode: `comfort` or `eco` as the program changes period, `boost` during a boost. During a drying run it reads `eco`, and after a change of mode or of boost it can show the old level for one or two polls. Experimental: read on one unit in a comfort period, an eco period, a boost and a drying run (issue #202). If you run a dryer in PROG, please [report](https://github.com/laberning/home_io_control/issues) whether the sensor follows the display |
| Level heater | The level the heater reports, including `secured`, which a heater reports and a controller cannot set |
| Pilot-wire interface | Not available: its status carries no level (issue #228), so a `heating_level:` block on it is a configuration error |
| Radiator with a setpoint, heater with a light | Not available: the hub reads no level from them, so a `heating_level:` block on them is a configuration error. If you run one in its program and it should report a level, please [open an issue](https://github.com/laberning/home_io_control/issues) so it can be checked |

The sensor has no state until the heater has reported a level. A read that goes unanswered, or a
reply with a value that is not in the list, leaves the last value shown.
[ADR 0091](adr/0091-every-climate-preset-is-a-command-and-the-heating-level-is-a-sensor.md) records
why the level is a sensor and not a preset.

## Vendor terms

| Term | Where you see it | In Home Assistant |
|---|---|---|
| BASIC | AGILIA IO display: holds the set temperature | `heat` |
| MANUEL | Adelis display: holds the set temperature | `heat` |
| PROG | Display of a radiator or towel dryer: follows the weekly program | `auto`, with the `Program` preset on a radiator |
| AUTO | AGILIA IO display: learns when the room is used | `auto` with the `Auto` preset |
| AUTO | Calypso display: the heater adapts to your consumption | `eco` |
| ECO on, ECO off | Calypso display, in manual mode: whether the electric element may assist | `heat_pump`, `performance` |
| Comfort (Confort) | The temperature a heater holds in normal use, and a pilot-wire order | The target in `heat`; the `comfort` preset of a level heater; `comfort` on the level sensor |
| Comfort -1, Comfort -2 | Pilot-wire orders: one or two degrees below comfort | The `Comfort -1` and `Comfort -2` presets |
| Eco | The lower temperature of a program, and a pilot-wire order | `eco` on the level sensor; the `eco` preset of a level heater |
| Frost protection (hors-gel, `H.Gel` on a Thermor display) | The lowest heating level | The `away` preset of a level heater; `frost protection` on the level sensor |
| Boost | A timed run on a towel dryer or a water heater; a level on a level heater | The `boost` preset; on a water heater the `boost` switch |
| Drying | Adelis display: a timed run | The `Drying` preset |
| Dérogation | A temporary change to the running program | A target set in `auto`; `set_derogation` in the `heating_control` action |
| Secured | A level a heater reports and a controller cannot set | `secured` on the level sensor; no preset |
| Holiday, absence | Calypso display | The water heater card's away mode |

## See also

- [Heating and climate](configuration/climate.md)
- [Water heaters](configuration/water-heater.md)
- [Supported devices](supported-devices.md)
