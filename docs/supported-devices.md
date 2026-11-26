# Supported devices
<!-- doxygen-label: supported_devices -->

Which IO-Homecontrol devices this project has been run against and how each one got onto the hub.
If your device is not listed, that usually means nobody has reported it yet rather than that it
fails — see [Getting your device added](#getting-your-device-added).

## How to read the tables

| Status | Meaning |
|---|---|
| ✅ **Confirmed** | Worked end to end on real hardware. |
| ⚠️ **Partial** | Some operations work and others reproducibly do not. The notes say which. |
| 📣 **Reported** | A field report says it works. Not reproduced here, and no capture in the corpus. |
| 🔍 **Traffic captured only** | Frames from this device are decoded in the corpus, but this project has never controlled it. |
| ❌ **Unsupported** | Tried and failed. The cause is either understood or under investigation. |

"Not recorded" in the Route column means the report does not say how the device got onto the hub.

**Route** is how the device got onto the hub: [Discover & Pair](pairing.md),
[key extraction](key-extraction.md) from a hub that already controls it, or
[1W enrollment](configuration/oneway-transmit.md#enrolling-this-hub-as-a-controller) for a device
driven one-way. **Type** is the `io_device_type` to declare; the names are on
[Device types and manufacturers](device-types.md).

The notes name the evidence. "Corpus" entries are golden-frame recordings in
[tests/corpus/captures/](https://github.com/laberning/home_io_control/tree/main/tests/corpus/captures) —
real RF frames, replayed by the test suite, and the strongest kind of evidence here. Issue numbers
refer to the project's [issue tracker](https://github.com/laberning/home_io_control/issues). Where a
device has a page of its own, the details, the working YAML and the quirks are there.

A device's absence is not a verdict. The protocol is the same across the alliance's members, so an
unlisted cover-like actuator has a good chance of working with `io_device_type` set to the closest
match.

## Device matrix

### Somfy

| Device | Type | Status | Route | Notes |
|---|---|---|---|---|
| **Sunea IO awning motor** | `vertical_exterior_awning` | ✅ Confirmed | Discover & Pair | 31 corpus captures, all three radios. Over 1W: open, close, stop and the one-way cover's slider (timed stop and position frame) confirmed on a bench run. [Somfy Sunea IO](devices/somfy-sunea-io.md) |
| **Sunea IO 70/17**, **Sunea IO 40/17** | `roller_shutter` | ✅ Confirmed | Discover & Pair | [Somfy Sunea IO](devices/somfy-sunea-io.md) |
| **MAESTRIA+ IO** | `roller_shutter` | ✅ Confirmed | Key extraction; Discover & Pair did not succeed | Heltec V3.2 / SX1262. [Somfy Sunea IO](devices/somfy-sunea-io.md) |
| **Sunea IO screen** | `vertical_exterior_awning` | 🔍 Traffic captured only | — | Corpus `somfy_awning_exchange_priority_level_sx1276`, a query from a TaHoma Switch |
| **Awning actuator (IO Vertical)** | `vertical_exterior_awning` | ✅ Confirmed | Discover & Pair | Discovery only. Corpus `somfy_awning_discovery_lab_response` |
| **Horizontal awning** (two units) | `horizontal_awning` | ✅ Confirmed | Discover & Pair | Direction is inverted automatically. Corpus `somfy_awning_discovery_spe_paired_rollcall` |
| **Sunilus IO 50/12** | `roller_shutter` | ✅ Confirmed | Discover & Pair | Corpus `somfy_sunilus_pairing_key_transfer_rejected_error` |
| **OXIMO io**, **OXIMO 50 io** (wired) roller shutter motors | `roller_shutter` | ✅ Confirmed | Two-way pairing | Open, close, stop, position. Two motors on one Heltec V2 / SX1276 hub, issue #178. The OXIMO 50 io advertises a 5 ms turnaround and drops the exchange when the hub answers its challenge even 5 ms late |
| **Oximo 40 Solar tubular motor** | `roller_shutter` | 🔍 Traffic captured only | — | `low_power: true`. Corpus `somfy_oximo40_statuspoll_sx1262`, issue #45 |
| **RS100 IO** (mains) | `roller_shutter` | ✅ Confirmed | [Pairing by address](pairing.md#pairing-a-device-by-address); Discover & Pair got no answer | No `low_power:`, and a short `status_poll_interval`. Five motors on a LilyGO T-Beam v1.2 / SX1276, [pull request #258](https://github.com/laberning/home_io_control/pull/258). [Somfy RS100 IO](devices/somfy-rs100.md) |
| **RS100 Solar** | `vertical_exterior_awning` / `roller_shutter` | ⚠️ Partial | Discover & Pair; retry if the key exchange stalls | `low_power: true`. One motor runs on 1W only (issue #187). [Somfy RS100 IO](devices/somfy-rs100.md) |
| **Sonesse 40 io** (interior blind motor) | `vertical_interior_blind` | 📣 Reported | Discover & Pair | Commands and status reads without a failed exchange in a logged session. Heltec V4.3, issue #196 |
| **J406 IO shutter motor**, also "J406io Protect" | `exterior_venetian_blind` | ✅ Confirmed | Discover & Pair | Enumerates as `0x11`. Corpus `somfy_j406_discovery_1w_overheard` |
| **Exterior venetian blinds** (ten units, model not given) | `exterior_venetian_blind` | 📣 Reported | Key extraction from a TaHoma Switch | Open, close, stop, position and tilt; their Somfy io remotes and a Situo 1 io Pure II as `linked_remotes`. LilyGO T3 V2.1 / SX1276, issue #244 |
| **Pergola io motor** | `horizontal_awning` | ✅ Confirmed | Discover & Pair | Issue #121. [Somfy pergola](devices/somfy-pergola.md) |
| **Bioclimatic pergola louvers** | `bioclimatic_pergola` | ✅ Confirmed | Key extraction | `sx1262_rx_bandwidth: 156.2` on SX1262. Pull request #157. [Somfy pergola](devices/somfy-pergola.md) |
| **Sunea SCR 40 io** | not reported | ✅ Confirmed | Discover & Pair | Open, close, position. Issue #143. [Somfy pergola](devices/somfy-pergola.md) |
| **IO patio-roof motor** | `horizontal_awning`, 1W | ✅ Confirmed | 1W enrollment; Discover & Pair got no answer | Enrollment only; commands afterwards not reported. Issue #147. [Somfy pergola](devices/somfy-pergola.md) |
| **MOTO 1000 io garage drive** (sold by Wisniowski) | `garage_opener`, subtype 58 | 📣 Reported | Discover & Pair | Open, close, stop, Partial Position; no percent positions. Issue #163. [Garage and gate openers](devices/somfy-garage-gate-openers.md) |
| **Dexxo Smart io 800 garage door opener** | `garage_opener`, subtype 58 | 📣 Reported | Key extraction from a TaHoma Switch | Open, close, stop, built-in light. Issue #244. [Garage and gate openers](devices/somfy-garage-gate-openers.md) |
| **Elixo 3S io swing gate opener** | `gate_opener`, subtype 58 | 📣 Reported | Key extraction from a TaHoma Switch | Open, close, stop, light output. Issue #244. [Garage and gate openers](devices/somfy-garage-gate-openers.md) |
| **Izymo IO dimmer** | `light` | ✅ Confirmed | Discover & Pair | `dimmable: true`. Corpus captures on all three radios. [Somfy Izymo IO dimmer](devices/somfy-izymo-dimmer.md) |
| **Izymo IO dimmer, driven over 1W** | `light`, 1W | ✅ Confirmed | 1W enrollment | On and off confirmed; the one-way light entity with `dimmable: true` confirmed in both slider ways (`timed`, `frame`), 50 of 50 each, read back over two-way. [Somfy Izymo IO dimmer](devices/somfy-izymo-dimmer.md) |
| **Izymo io on/off receiver** | `on_off_switch`, subtype 2 | 📣 Reported | Discover & Pair | Switches on and off, follows a linked Situo 1 io Pure II. SX1262 module, [issue #57](https://github.com/laberning/home_io_control/issues/57) |
| **White LED Receiver io** (4-output LED dimmer) | `light` | ✅ Confirmed | Discover & Pair, once per output | `dimmable: true`. Issues #119, #143. [Somfy White LED Receiver io](devices/somfy-white-led-receiver.md) |

Tilt control is confirmed on a tilt-capable Somfy blind, with 8 corpus captures.

Somfy motors are also sold under other brands. The two awnings this project was originally built
for carry a "Heim & Haus" badge over a Somfy Sunea IO motor, so match your motor to a row by what
is inside the tube rather than by the name on the awning.

`low_power: true` on the solar Somfy motors is a reasoned starting point rather than a confirmed
setting: the option is meant for battery and solar actuators, and it is confirmed on VELUX hardware
through issue #87, but no RS100 Solar or Oximo report has compared it against `false` on the same
motor. A mains RS100 IO is the other case: it reports `power_save=always_alive` in the roll-call and
answers worse with the flag set, so set the option from what the roll-call reports rather than from
the model name.
Over 1W the same applies: a battery or solar one-way device needs `low_power: true`. Without it the
hub sends four short copies, which a mains-powered receiver obeys and a sleeping one misses
([ADR 0122](adr/0122-an-unset-one-way-low-power-sends-four-short-copies.md),
[Sending 1W commands](configuration/oneway-transmit.md#what-one-command-puts-on-the-air)).

### VELUX

All VELUX rows are described on [VELUX INTEGRA and the KLR/KLF/KUX family](devices/velux-integra.md).

| Device | Type | Status | Route | Notes |
|---|---|---|---|---|
| **INTEGRA roof-window actuator** | `window_opener` | ✅ Confirmed | Not recorded | Adds a Ventilation Position button; `rain_sensor_poll_interval:` adds a Rain sensor. 5 corpus captures, issues #98, #242 |
| **INTEGRA roller shutter, mains-wired** | `roller_shutter` | 📣 Reported | Not recorded | A 230 V installation, driven alongside two KLR 200s. Issue #57 |
| **SSL solar roller shutter** | `roller_shutter` | ⚠️ Partial | Discover & Pair right after a motor-side reset, or the key of a KLR 200 | `low_power: true`. `stop` reaches it mid-travel, but not on every attempt. [SSL solar roller shutter](devices/velux-integra.md#ssl-solar-roller-shutter), issue #240 |
| **MSU 100100 5070WL solar awning screen** | `vertical_exterior_awning` | ⚠️ Partial | Not recorded | `low_power: true`. Open and close work; `stop` has no effect while it moves |
| **INTEGRA SOLAR blinds** | `vertical_interior_blind` | 📣 Reported | Not recorded | `low_power: true`. No capture |
| **SML electric roller shutter** | `roller_shutter`, 1W or two-way | ✅ Confirmed over 1W (SX1276); 📣 Reported two-way (SX1262) | 1W enrollment, or Discover & Pair after Gear on its KLI 310 | [SML electric roller shutter](devices/velux-integra.md#sml-electric-roller-shutter), issues #17, #230 |
| **Interior blinds behind a KLI 312** | `interior_venetian_blind`, 1W | ✅ Confirmed | 1W enrollment | Open, close, stop (SX1262). `low_power: true` and two `enrollment_classes` |
| **FSC solar pleated blind** (PK10) | 1W | 📣 Reported | 1W enrollment; Discover & Pair got no answer | Open, close, stop. Heltec V4.2, issue #74 |
| **INTEGRA CVP roof window (060090)** behind a KLI 311 | `window_opener`, 1W | 📣 Reported | 1W enrollment; Discover & Pair got no answer | Enrollment only; commands afterwards not reported. Issue #249 |
| **KUX 100 / PK03 / PK04 wired bridge** | Bridge | 🔍 Traffic captured only | — | 3 corpus captures of KLR 200 ↔ KUX 100 traffic |

Devices behind an extracted VELUX key are often solar or battery powered. Scan Paired Devices
reaches them without any setting; a directed command needs `low_power: true` on the device's entry.

### SIMU

| Device | Type | Status | Route | Notes |
|---|---|---|---|---|
| **T3.5 E BHz DC solar tubular motor** (Autosun 2 BHz) | `roller_shutter` | ✅ Confirmed | Discover & Pair after PROG on its BHz transmitter | `low_power: true`. Six motors, issues #138, #149. [SIMU T3.5 E BHz DC](devices/simu-t35-bhz.md) |

SIMU also sells a **T3.5 EHz DC**, without the B. That motor uses SIMU-Hz at 433 MHz, not
io-homecontrol, so this project cannot control it. Check the motor label or its box for "E BHz"
before pairing; the [device page](devices/simu-t35-bhz.md#check-the-motor-first) has the details.

### Heating and hot water

Electric heaters, towel dryers, water heaters and heat pumps of the Atlantic group (Atlantic,
Thermor, Sauter). What each one's card offers in Home Assistant is on
[Heaters and water heaters in Home Assistant](heating.md).

| Device | Kind | Status | Route | Notes |
|---|---|---|---|---|
| **Atlantic AGILIA IO** (electric radiator, display model) | Radiator with a setpoint | ✅ Confirmed | Discover & Pair from the radiator's menu | Three radiators, issue #165. [Atlantic AGILIA IO](devices/atlantic-agilia-io.md) |
| **Thermor EMOTION 3 H** (electric radiator) | Radiator with a setpoint | ✅ Confirmed | Discover & Pair from the radiator's menu | Two radiators, issue #228. Set a target in `heat`. [Thermor EMOTION 3 H](devices/thermor-emotion-3h.md) |
| **Sauter Hekla** (electric heater with a night light) | `io_model: heater_with_light` | ✅ Confirmed | Discover & Pair from the heater's menu | Three heaters, alongside a Cozytouch, issue #222. [Sauter Hekla](devices/sauter-hekla.md) |
| **Sauter Tahual** (electric radiator) | Radiator with a setpoint | ⚠️ Partial | Not recorded | Reports `0x34/1`, so it is declared like the AGILIA IO, with no `io_model`. Only the energy total is reported, read by the contributor's own build of the `energy:` sensor and equal to the display on two radiators ([pull request 238](https://github.com/laberning/home_io_control/pull/238), corpus `sauter_tahual_probe_energy_register`). See [Energy sensor](configuration/climate.md#energy-sensor) |
| **Atlantic Adelis** (towel dryer, "ADELIS 2 DIGIT" and "ADELIS 2 M") | `io_model: towel_dryer` | ✅ Confirmed | Discover & Pair from the dryer's menu | Three units, issues #202, #243. [Atlantic Adelis](devices/atlantic-adelis.md) |
| **Thermor Interface Cozytouch for pilot-wire heaters** (ref. 450 251) | `io_model: pilot_wire_interface` | ✅ Confirmed | The key of its Cozytouch installation; it cannot be paired on its own | All six orders work; no level is reported back. `low_power: true`. Issue #228. [Thermor Interface Cozytouch](devices/thermor-pilot-wire-interface.md) |
| **Atlantic Tatou Pilotage Intelligent Connecté** (electric radiator) | — | ❌ Unsupported | Discover & Pair gets no answer | Its key has not been retrieved. Try the experimental [`pairing_discovery_send_channels: all`](configuration/tuning.md#pairing_discovery_send_channels). SX1276 board, issue #199 |
| **Atlantic Alfea Duo Extensa A.I.** (air-to-water heat pump) | `sensor:` entry | ⚠️ Partial | The key of the installation it belongs to | Reads the heating and hot-water energy counters and, experimentally, the outdoor temperature, alongside the installation's TaHoma; writes nothing. Issue #244. [Atlantic Alfea heat pump](devices/atlantic-heat-pump.md) |
| **Atlantic Calypso Connecté** (heat-pump water heater) | Water heater | ✅ Confirmed | Discover & Pair | Readings, target, mode, boost and away; takes only its own fixed targets. Issue #198. [Atlantic Calypso Connecté](devices/atlantic-calypso.md) |
| **Atlantic Explorer V4** (heat-pump water heater) | `io_model: explorer_v4` | ✅ Confirmed | Pairing | Readings, target, mode, boost and away on two units. Pull request #252 and issue #198. [Atlantic Explorer V4](devices/atlantic-explorer-v4.md) |
| **Sauter Guelma connecté** (electric storage water heater, 80 L) | `io_model: electric_water_heater` | ✅ Confirmed | Not recorded | One heater, issue #247. [Sauter Guelma connecté](devices/sauter-guelma.md) |

### Hubs you can take a key from

A vendor hub you already own hands its installation key to this component, and keeps working
alongside it. [Key extraction](key-extraction.md) describes the procedure.

| Hub | Status | What to press on the hub | Notes |
|---|---|---|---|
| **Somfy TaHoma Switch** | ✅ Confirmed | Its "add a device" wizard | Corpus `somfy_tahoma_pairing_key_extraction_success_sx1276` |
| **Somfy Connectivity Kit** | ⚠️ Partial | Its "add a device" wizard | The extraction stalled. Corpus `somfy_connectivity_kit_pairing_key_extraction_stall` |
| **VELUX KLR 200 control pad** | ✅ Confirmed | "Register product" | First attempt, including the node verification round. Corpus `velux_klr200_pairing_key_extraction_success` |
| **VELUX KLR 300 control pad** | ✅ Confirmed | "Search for new products" | One tester on an SX1276 board, first attempt. Corpus `velux_klr300_pairing_key_extraction_success` |
| **VELUX KLR 100 control pad** | 📣 Reported | "Register product" | Issue #87. Its Copy system answers a key request too, reported with another controller (issue #242) |
| **VELUX KLF 200 gateway** | 📣 Reported | **SEARCH FOR PRODUCTS**, repeated while the switch is armed | One reporter, LilyGO T-ETH-Elite / SX1276, issue #195. COPY PRODUCTS is not a route |
| **VELUX KIG 300 hub** | ⚠️ Partial | Not recorded | One extraction succeeded, one stalled. 4 corpus captures |
| **Freebox Delta** | ✅ Confirmed | "Share your network" in the Freebox app | Uses Discover & Pair with `pairing_key_transfer: pull`, not the Recover System Key switch. Issue #215. [Freebox Delta](devices/freebox-delta.md) |

The connected radiators of the Atlantic group need no hub for this: each hands over its key from
its own menu; see [Devices that hand over their own key](pairing.md#devices-that-hand-over-their-own-key).

### Remotes and sensors you can listen to

One-way senders the hub overhears. Add one to an entity's `linked_remotes:` to keep its state fresh,
or to `exposed_senders:` to fire Home Assistant events; see
[Linked remotes and sender events](configuration/remotes.md).

| Device | Status | Notes |
|---|---|---|
| **Somfy Smoove IO wall switch and remote** | ✅ Confirmed | Also re-pairs a reset Izymo. 8 corpus captures, corpus `somfy_smoove_enrollment_monitor_sx1276`, issue #27 |
| **Somfy Situo 1 io** | ✅ Confirmed | Its PROG hold decodes as an add-controller frame to `00003F` followed by two `0x20` frames, and opens the motor's 1W learning window (issue #147) |
| **Somfy Situo 5 io Pure II** | ✅ Confirmed | Its PROG hold and EXECUTE frames decode, and a Somfy awning paired from that gesture |
| **Somfy KeyGo io** (4-button remote) | 📣 Reported | Each button has its own sender address. Issue #244 |
| **Somfy combined wind/rain protection station** | ✅ Confirmed | 2 corpus captures |
| **Somfy Eolis 3D WireFree IO wind sensor** | 📣 Reported | A standalone wind sensor that links directly to its awning motor |
| **SIMU BHz wall transmitter** (1 channel) | ✅ Confirmed | Its PROG press decodes as an add-controller frame and opens the motor's two-way pairing window |
| **VELUX KLI 310 universal wall remote** | 🔍 Traffic captured only | Corpus `velux_kli310_discovery_alt_sweep` |
| **VELUX KLI 313 remote** (ships with the SML roller shutter) | 🔍 Traffic captured only | 3 corpus captures |
| **VELUX KLI 312 interior-blind remote** | 📣 Reported | Its Gear press is reported from a log; no corpus capture |

### Other manufacturers

Honeywell, Hörmann, Assa Abloy, Niko, WindowMaster, Renson, CIAT, Secuyou and Overkiz are names the
schema accepts, not devices anyone has reported. Only `somfy` and `velux` have a verified 1W wire
profile; see [Named manufacturers](device-types.md#named-manufacturers).

## Device pages

These devices have enough evidence behind them to be worth a page of their own.

<!-- doxygen-subpages -->
- [Somfy Izymo IO dimmer](devices/somfy-izymo-dimmer.md)
- [Somfy Sunea IO](devices/somfy-sunea-io.md)
- [Somfy RS100 IO](devices/somfy-rs100.md)
- [Somfy White LED Receiver io](devices/somfy-white-led-receiver.md)
- [Somfy pergola and patio-roof motors](devices/somfy-pergola.md)
- [Somfy garage and gate openers](devices/somfy-garage-gate-openers.md)
- [VELUX INTEGRA and the KLR/KLF/KUX family](devices/velux-integra.md)
- [SIMU T3.5 E BHz DC solar shutter motor](devices/simu-t35-bhz.md)
- [Atlantic AGILIA IO electric radiator](devices/atlantic-agilia-io.md)
- [Thermor EMOTION 3 H electric radiator](devices/thermor-emotion-3h.md)
- [Atlantic Adelis towel dryer](devices/atlantic-adelis.md)
- [Sauter Hekla electric heater](devices/sauter-hekla.md)
- [Thermor Interface Cozytouch for pilot-wire heaters](devices/thermor-pilot-wire-interface.md)
- [Atlantic Calypso Connecté water heater](devices/atlantic-calypso.md)
- [Atlantic Explorer V4 water heater](devices/atlantic-explorer-v4.md)
- [Sauter Guelma connecté water heater](devices/sauter-guelma.md)
- [Atlantic Alfea heat pump](devices/atlantic-heat-pump.md)
- [Freebox Delta](devices/freebox-delta.md)
<!-- /doxygen-subpages -->

## Getting your device added

A report is welcome whether the device worked or not — a clean failure is as useful as a success,
and several rows above started as failures.

Open an issue with:

- **The device**: vendor, model, and the catalogue number if you have it.
- **What you tried**: Discover & Pair, key extraction, or 1W enrollment.
- **What happened**: paired and moves, paired but nothing moves, never found, and so on.
- **Your board and radio**: for example Heltec V3 with `radio_type: sx1262`.
- **The "Last Pairing Result" sensor**, if pairing got that far. It carries the device type, the
  node ID, and a machine-readable outcome — see [Pairing](pairing.md).
- **Logs** around the attempt, if you can capture them. Build with `-DIOHOME_FRAME_LOG` as well
  as `DEBUG` logging. Both settings are in
  [Reporting unsupported devices](contributing.md#reporting-unsupported-devices).

If the device is listed above with a status you can improve on — you got a 📣 Reported row working,
or you have a capture for a 🔍 row — that is worth an issue too. It is how rows move up.

## See also

- [Getting started](getting-started.md) — the whole path from a board to a working entity
- [Pairing](pairing.md) — Discover & Pair, for a device no hub has claimed
- [Key extraction](key-extraction.md) — the route when a hub already controls it
- [Device types and manufacturers](device-types.md) — the names `io_device_type` accepts
