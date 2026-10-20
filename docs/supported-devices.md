# Supported devices
<!-- doxygen-label: supported_devices -->

Which IO-Homecontrol devices this project has been run against, what worked, and what the evidence
for each entry is. If your device is not listed, that usually means nobody has reported it yet
rather than that it fails — see [Getting your device added](#getting-your-device-added).

## How to read this table

Every row carries its evidence, so you can judge how much weight to put on it. The status words
mean:

| Status | Meaning |
|---|---|
| ✅ **Confirmed** | Worked end to end on real hardware. |
| ⚠️ **Partial** | Some operations work and others reproducibly do not. The row says which. |
| 📣 **Reported** | A field report says it works. Not reproduced here, and no capture in the corpus. |
| 🔍 **Traffic captured only** | Frames from this device are decoded in the corpus, but this project has never controlled it. |
| ❌ **Unsupported** | Tried and failed. The cause is either understood or under investigation. |

"Corpus captures" are golden-frame recordings in
[tests/corpus/captures/](https://github.com/laberning/home_io_control/tree/main/tests/corpus/captures) —
real RF frames, replayed by the test suite. A row backed by captures is the
strongest kind of evidence here.

A device's absence is not a verdict. The protocol is the same across the alliance's members, so an
unlisted cover-like actuator has a good chance of working with `io_device_type` set to the closest
match.

## Device matrix

### Somfy — actuators

| Device | Type | Status | Route that worked | Key config | Evidence |
|---|---|---|---|---|---|
| **Sunea IO awning motor** | `vertical_exterior_awning` | ✅ Confirmed | Discover & Pair | `io_device_type: vertical_exterior_awning` | 31 corpus captures, across all three radios |
| **Sunea IO 70/17** | `roller_shutter` | ✅ Confirmed | Discover & Pair | — |  |
| **Sunea IO 40/17** | `roller_shutter` | ✅ Confirmed | Discover & Pair | — |  |
| **Sunilus IO 50/12** | `roller_shutter` | ✅ Confirmed | Discover & Pair | — | corpus `somfy_sunilus_pairing_key_transfer_rejected_error` |
| **Izymo IO dimmer** | `light` | ✅ Confirmed, including dimming | Discover & Pair, including repeated reset-and-repair cycles | `dimmable: true` | 39 corpus captures; the `light:` platform's hardware validation |
| **J406 IO shutter motor** | `exterior_venetian_blind` | ✅ Confirmed | Discover & Pair | `io_device_type: exterior_venetian_blind` (enumerates as `0x11`) | Corpus `somfy_j406_discovery_1w_overheard`, "J406io Protect" is the same, works too |
| **MAESTRIA+ IO** | `roller_shutter` | ✅ Confirmed | **Key extraction** — Discover & Pair did not succeed | `accept_foreign_pairing: true` | on a Heltec V3.2 / SX1262 |
| **Horizontal awning** (two units) | `horizontal_awning` | ✅ Confirmed | Discover & Pair | Direction inversion is applied automatically for this family | Corpus `somfy_awning_discovery_spe_paired_rollcall` |
| **IO patio-roof motor** (logs as a "Somfy IO motor", behaves as a horizontal awning) | `horizontal_awning`, 1W | ✅ Confirmed — enrollment (the roof moved on Enroll); commands afterwards not reported | 1W enrollment: hold PROG on the existing Situo 1 until the roof jogs, then press Enroll. Two-way Discover & Pair got no answer | `manufacturer: somfy`, `execute_broadcast: all` | SX1276 board, issue #147. The full Somfy enrollment gesture goes to `00003F` |
| **Pergola io motor** | `horizontal_awning` | ✅ Confirmed | Discover & Pair, after a single PROG hold on its Situo | — | Heltec V3 / SX1262, issue #121. For the kit's LED lighting see [Pairing a pergola kit](devices/somfy-white-led-receiver.md#pairing-a-pergola-kit) |
| **Bioclimatic pergola louvers** (TaHoma: "Pergola louver IO") | `bioclimatic_pergola` (`0x1D`) | ✅ Confirmed — position, open, close, stop | **Key extraction** | `io_device_type: bioclimatic_pergola`; `sx1262_rx_bandwidth: 156.2` on SX1262 | Heltec V3 / SX1262, PR #157. At the default 58.6 kHz about one reply in four is missed. Stops about 1 % off target; reports no position after a STOP. Corpus `somfy_pergola_louver_probe_get_info2_sx1262`, `somfy_pergola_louver_exchange_open_close_position_stop_sx1262` |
| **White LED Receiver io** (4-output LED dimmer, logs as `LightVar_Wh_io`) | `light` | ✅ Confirmed, including dimming | Discover & Pair once per output: hold PROG about 2 s on the Situo channel for that output, then press Discover & Pair | `dimmable: true`; `sx1262_rx_bandwidth: 156.2` on SX1262 | Each output is a separate device. Heltec V3 / SX1262, issues #119, #143. Step-by-step: [Somfy White LED Receiver io](devices/somfy-white-led-receiver.md) |
| **Sunea SCR 40 io** | — | ✅ Confirmed — open, close, position | Discover & Pair | — | Pergola kit, alongside a White LED Receiver io; keeps working at `sx1262_rx_bandwidth: 156.2`. Heltec V3 / SX1262, issue #143 |
| **Awning actuator (IO Vertical)** | `vertical_exterior_awning` | ✅ Confirmed for discovery | Discover & Pair | — | Corpus `somfy_awning_discovery_lab_response` |
| **RS100 IO / RS100 Solar** | `vertical_exterior_awning` / `roller_shutter` | ⚠️ Partial — pairing needs a retried key exchange | Discover & Pair, retried | `low_power: true` | 7 corpus captures including both a success and a key-transfer stall |
| **MOTO 1000 io garage drive** (sold by Wisniowski) | `garage_opener` | 📣 Reported — Discover & Pair, status, linked remotes and STOP work; open and close need the main-parameter frame, which is waiting for a test | Discover & Pair | `io_device_type: garage_opener`, `low_power: true` | Heltec V3 / SX1262, issue #163. The regular position frame is refused with `PARAMETER_UNKNOWN` (corpus `wisniowski_moto1000_exchange_execute_parameter_unknown`); garage openers now get the shorter frame. FAVORITE is not supported on garage openers |
| **Oximo 40 Solar tubular motor** | `roller_shutter` | 🔍 Traffic captured only | — | `low_power: true` | Corpus `somfy_oximo40_statuspoll_sx1262`, issue #45 |
| **Sunea IO screen** | `vertical_exterior_awning` | 🔍 Traffic captured only | — | — | Corpus `somfy_awning_exchange_priority_level_sx1276` — a directed priority-level query from a real TaHoma Switch |
| **Tilt-capable blind/shutter** | tilt-capable cover | ✅ Confirmed for tilt control | — | — | 8 corpus captures |
| **Combined wind/rain protection station** | sensor (1W) | ✅ Confirmed for listening | Overheard; no pairing involved | `exposed_senders:` / `linked_remotes:` | 2 corpus captures |
| **Eolis 3D WireFree IO wind sensor** | sensor (1W) | 📣 Reported — listen-only | Overheard; no pairing involved | `exposed_senders:` / `linked_remotes:` | A standalone wind sensor, distinct from the combined station above; links directly to its awning motor. |

Somfy motors are also sold under other brands. The two awnings this project was originally built
for carry a "Heim & Haus" badge over a Somfy Sunea IO motor, so match your motor to a row by what
is inside the tube rather than by the name on the awning.

`low_power: true` on the solar Somfy motors is a reasoned starting point rather than a confirmed
setting: the option is meant for battery and solar actuators, and it is confirmed on VELUX hardware
through issue #87, but no RS100 or Oximo report has confirmed it yet.

### Somfy — hubs and remotes

These are useful as key-extraction sources or as senders you can listen to, not as things this
project controls.

| Device | Role | Status | Evidence |
|---|---|---|---|
| **Smoove IO wall switch** | 1W remote | ✅ Confirmed for listening, and for re-pairing a reset Izymo | 8 corpus captures |
| **TaHoma Switch** | Third-party hub | ✅ Confirmed as a key-extraction source | Corpus `somfy_tahoma_pairing_key_extraction_success_sx1276` |
| **Connectivity Kit** | Third-party hub | ⚠️ Partial — extraction stalled | Corpus `somfy_connectivity_kit_pairing_key_extraction_stall` |
| **Smoove IO remote** | 1W remote | ✅ Confirmed for listening | Corpus `somfy_smoove_enrollment_monitor_sx1276`; issue #27 |
| **Situo 1 io** | 1W remote | ✅ Confirmed for listening | Its PROG hold decodes as an io add-controller frame to `00003F`, followed by two `0x20` frames; it opens the motor's 1W learning window (issue #147) |
| **Situo 5 io Pure II** | 1W remote | ✅ Confirmed for listening | Its PROG hold and its EXECUTE frames both decode, and a Somfy awning paired from that gesture |

### VELUX

| Device | Type | Status | Route that worked | Key config | Evidence |
|---|---|---|---|---|---|
| **KLR 200 two-way control pad** | Third-party hub | ✅ Confirmed as a key-extraction source | Key extraction | `accept_foreign_pairing: true` | succeeded end to end on the first attempt, including the node verification round that follows it; corpus `velux_klr200_pairing_key_extraction_success` |
| **KLR 300 two-way control pad** | Third-party hub | ✅ Confirmed as a key-extraction source | Key extraction: start the KLR 300's own "search for new products" while the hub is armed | `accept_foreign_pairing: true` | One community tester on an SX1276 board, first attempt; the node verification round that follows also ran. Corpus `velux_klr300_pairing_key_extraction_success` and `velux_klr300_discovery_rollcall_node_verification` |
| **KLR 100 two-way control pad** | Third-party hub | 📣 Reported as a key-extraction source | Key extraction | `accept_foreign_pairing: true` | Its "Register product" gesture opens the extraction window the same way the KLR 200's does; issue #87 |
| **KIG 300 hub** | Third-party hub | ⚠️ Partial — one extraction succeeded, one stalled | Key extraction | `accept_foreign_pairing: true` | 4 corpus captures |
| **INTEGRA roof-window actuator** | `window_opener` | ✅ Confirmed | — | `io_device_type: window_opener`, which adds the Ventilation Position button; `rain_sensor_poll_interval:` adds a Rain sensor for a wired rain sensor | 5 corpus captures (probe replies, a dry and a rain-limited limitation read, and private-function sweeps dry and in the rain state); issue #98 |
| **INTEGRA roller shutter, mains-wired** | `roller_shutter` | 📣 Reported | — | — | A 230 V installation — two window actuators and their shutters, driven alongside two KLR 200s; issue #57. Mains power is what separates this from the solar rows below |
| **Devices behind an extracted KLR200 key** | Mixed | ✅ Confirmed | Key extraction, then `scan_paired_devices` | **`low_power: true`** on the affected devices | `scan_paired_devices` runs a low-power pass built to reach sleeping devices without any YAML declaration; `low_power: true` is still needed for directed commands once they are registered |
| **MSU 100100 5070WL solar awning screen** | `vertical_exterior_awning` | ⚠️ Partial — open and close work; `stop` has no effect while it moves | — | `low_power: true` | Nothing the hub sends gets an answer while the motor runs; stop it mid-travel with its own remote — see [VELUX INTEGRA](devices/velux-integra.md) |
| **SSL solar roller shutter** | `roller_shutter` | ⚠️ Partial — Discover & Pair, open, close, position and status; `stop` reaches it mid-travel, but not on every attempt | Discover & Pair right after a motor-side reset | `pairing_discovery_preamble: 32` (required); `pairing_discovery_destination: "0x00003F"` and `pairing_discovery_ack_capable: true` complete the confirmed setup but are not known to be required; `low_power: true` on the device entry | The reset removes the wall remote, so register it again afterwards. Press stop again if the shutter keeps moving, or use that remote. Step-by-step: [VELUX INTEGRA](devices/velux-integra.md#ssl-solar-roller-shutter) |
| **INTEGRA SOLAR blinds** | `vertical_interior_blind` | 📣 Reported | — | `low_power: true` | Named as a canonical low-power case; no capture |
| **KLI 313 remote** (ships with the SML roller shutter) | 1W remote | 🔍 Traffic captured only | — | — | 3 corpus captures |
| **KLI 310 universal wall remote** | 1W remote | 🔍 Traffic captured only | — | — | Corpus `velux_kli310_discovery_alt_sweep` |
| **KLI 312 interior-blind remote** | 1W remote | 🔍 Traffic reported only | — | — | Gear `0x2E` goes to `vertical_interior_blind` and `interior_venetian_blind`; no corpus capture |
| **KUX 100 / PK03 / PK04 wired bridge** | Bridge | 🔍 Traffic captured only | — | — | 3 corpus captures of KLR200 ↔ KUX100 traffic |
| **KLF 200** | Third-party hub | 📣 Reported as a key source | — | — | Named in several issue threads; no capture |
| **SML electric roller shutter** | `roller_shutter`, 1W | ✅ Confirmed — enrollment, open, close, stop (SX1276) | 1W enrollment: Gear on the existing KLI 310, then Enroll. Two-way Discover & Pair finds nothing (issue #17) | `manufacturer: velux`, `execute_broadcast: all`, `enrollment: true`, `low_power: false` | Mains-fed, e.g. through a KUX 110 power supply, which has no radio role; see [Sending 1W commands](configuration/oneway-transmit.md) |
| **Interior blinds behind a KLI 312** | `interior_venetian_blind`, 1W | ✅ Confirmed — enrollment, open, close, stop (SX1262) | 1W enrollment: Gear on the existing KLI 312, then Enroll | `manufacturer: velux`, `execute_broadcast: all`, `enrollment: true`, `enrollment_classes: [vertical_interior_blind, interior_venetian_blind]`, `low_power: true` |  |

### SIMU

| Device | Type | Status | Route that worked | Key config | Evidence |
|---|---|---|---|---|---|
| **T3.5 E BHz DC solar tubular motor** (Autosun 2 BHz) | `roller_shutter` | ✅ Confirmed on 6 motors — Discover & Pair, open, close, live position, device name | Discover & Pair: hold PROG on the BHz transmitter that already controls the motor, release it at the first jog, then press Discover & Pair. A three-jog gesture also worked; see [the device page](devices/simu-t35-bhz.md#pairing-a-simu-shutter-motor) | `low_power: true` | Reports its name as `T3.5EBHZ DC`. Heltec V3 / SX1262, issues #138, #149. Step-by-step: [SIMU T3.5 E BHz DC](devices/simu-t35-bhz.md) |
| **BHz wall transmitter** (1 channel) | 1W remote | ✅ Confirmed for listening | — | — | Its PROG press decodes as an io add-controller frame and opens the motor's two-way pairing window |

SIMU also sells a **T3.5 EHz DC**, without the B. That motor uses SIMU-Hz at 433 MHz, not
io-homecontrol, so this project cannot control it. Check the motor label or its box for "E BHz"
before pairing; the [device page](devices/simu-t35-bhz.md#check-the-motor-first) has the details.

### Other vendors

| Device | Status | Evidence |
|---|---|---|
| **AGILIA IO electric radiator** (Atlantic, display model) | ✅ Confirmed on three radiators — Discover & Pair from the display's "Paramètres" → "Connectivité" → "Connexion" menu, target temperature, Off / Heat / Auto / `Program`, room temperature and heating / idle, all read back. Each radiator hands over its own key, and its heater sits one address above the node that pairs. `home` / `away` and the window entities are not tested yet. Step-by-step: [Atlantic AGILIA IO](devices/atlantic-agilia-io.md) | Heltec V3 / SX1262, issue #165; corpus `atlantic_agilia_*` (pairing, roll-call, register reads and writes) |
| **Atlantic Thermor-style climate/heating actuator** | 🔍 Traffic captured only — the `climate:` platform is built against it but has never run on hardware | Corpus `atlantic_thermor_exchange_write_private_param` |
| **Honeywell, Hörmann, Assa Abloy, Niko, WindowMaster, Renson, CIAT, Secuyou, Overkiz, Atlantic Group** | Untested — these are names the schema accepts, not devices anyone has reported | Only `somfy` and `velux` have a verified 1W wire profile |

## Naming reference

### Named device types

Both `io_device_type` and the `class:<device_type>` form of `linked_remotes` accept these named
values:

| Name | Hex ID | Name | Hex ID |
|---|---|---|---|
| `interior_venetian_blind` | `0x01` | `horizontal_awning` | `0x10` |
| `roller_shutter` | `0x02` | `exterior_venetian_blind` | `0x11` |
| `vertical_exterior_awning` | `0x03` | `louvre_blind` | `0x12` |
| `window_opener` | `0x04` | `curtain_track` | `0x13` |
| `garage_opener` | `0x05` | `exterior_heating` | `0x15` |
| `light` | `0x06` | `intrusion_alarm` | `0x17` |
| `gate_opener` | `0x07` | `swinging_shutter` | `0x18` |
| `rolling_door_opener` | `0x08` | `sliding_window` | `0x1B` |
| `lock` | `0x09` | `bioclimatic_pergola` | `0x1D` |
| `vertical_interior_blind` | `0x0A` | `heat_pump` | `0x16` |
| `dual_roller_shutter` | `0x0D` | `electrical_heater` | `0x34` |
| `heating_temperature_interface` | `0x0E` | | |
| `on_off_switch` | `0x0F` | | |

A device type not in this table can still be declared as a raw hex ID, in either place:
`io_device_type: 0x14` or `linked_remotes: ["class:0x14"]`. Named and raw-hex entries can be mixed
freely within the same `linked_remotes` list.

**Finding your device's type:** the surest way is to pair it through Home Assistant and check the
"Last Pairing Result" diagnostic sensor — its `type=` field reports the device's actual type name,
e.g. `type=vertical_exterior_awning`. If you already know what the device is (say, a Somfy awning), use the matching
name from the table above.

### Node types and subtypes

Discovery, Scan Paired Devices and the pairing log name a device as `<type>, <subtype>` followed by
the wire values, for example `electrical heater, with adjustable setpoint (0x34/1)`; a subtype
without a name prints as `subtype N`. A subtype the table does not list behaves like its type. Subtype 58 means "main parameter only" on
every type: such a device takes position commands without the functional-parameter block.

Types marked "not supported yet" are recognised and named, but no platform drives them, so
discovery reports them and prints no YAML. Subtype names that have not been confirmed on a real
device are marked experimental.

| Type | Name | Named subtypes | Support |
|---|---|---|---|
| `0x01` | interior venetian blind | | Cover with tilt |
| `0x02` | roller shutter | 1 with adjustable slats, 2 with projection | Cover |
| `0x03` | vertical exterior awning | | Cover |
| `0x04` | window opener | 1 with integrated rain sensor | Cover, with a ventilation position |
| `0x05` | garage opener | 58 position only | Cover, main parameter only |
| `0x06` | light | 1 dimmable RGB (experimental), 2 tunable white (experimental), 58 on/off only | Light; subtypes 1 and 2 control brightness only (experimental) |
| `0x07` | gate opener | 58 position only | Cover |
| `0x08` | rolling door opener | | Cover |
| `0x09` | lock | 1 window lock (experimental), 2 car controller (experimental) | Lock (experimental); subtype 2 not supported yet |
| `0x0A` | vertical interior blind | | Cover with tilt |
| `0x0B` | secure configuration device | | Not supported yet |
| `0x0C` | beacon | | Not supported yet |
| `0x0D` | dual roller shutter | | Cover |
| `0x0E` | heating temperature interface | | Climate with a setpoint, off / heat / auto and comfort / eco / secured presets (experimental, untested); see [Heating and climate](configuration/climate.md#heating-temperature-interface-and-heat-pump) |
| `0x0F` | on/off switch | 2 micromodule (experimental) | Switch (experimental) |
| `0x10` | horizontal awning | 1 rail-guided pergola | Cover, direction inverted |
| `0x11` | exterior venetian blind | | Cover with tilt |
| `0x12` | louvre blind | | Cover with tilt |
| `0x13` | curtain track | | Cover |
| `0x14` | ventilation point | 1 air inlet, 2 air transfer, 3 air outlet | Not supported yet |
| `0x15` | exterior heating | 58 on/off only | Fan, a heat level of 0–100 % (subtype 58: on/off), experimental and unconfirmed on hardware; see [Exterior heating](configuration/fan.md) |
| `0x16` | heat pump | 1 heat pump (Atlantic) (experimental), 2 boiler (Atlantic) (experimental) | Climate for subtype 0 with a setpoint, off / heat and comfort / eco / setback / halted presets (experimental, untested); the Atlantic subtypes are not supported yet |
| `0x17` | intrusion alarm | | Not supported yet |
| `0x18` | swinging shutter | 1 with independent leaves, 58 position only (experimental) | Cover |
| `0x1B` | sliding window | 1 with lock (experimental) | Cover |
| `0x1C` | zone control | | Not supported yet |
| `0x1D` | bioclimatic pergola | | Cover |
| `0x1E` | siren | | Not supported yet |
| `0x33` | domestic hot water | | Not supported yet |
| `0x34` | electrical heater | 1 with adjustable setpoint | Climate: subtype 1 over private registers with read-back, confirmed on the [Atlantic AGILIA IO](devices/atlantic-agilia-io.md), with a Window sensor and an Open Window Detection switch (both experimental); the plain heater (subtype 0) with heating levels (experimental, untested) |
| `0x35` | heat recovery ventilation | | Not supported yet |
| `0xFF` | central house control | | Not supported yet |
| `0x3F0` | test and evaluation | | Not supported yet |
| `0x3FF` | remote controller | 2 Keygo, 3 Izymo, 4 public frame, 5 V500 (all experimental) | Not supported yet; see [pairing](pairing.md) for devices that answer discovery as a remote controller |

### Named manufacturers

The `manufacturer` key on a 1W `oneway_controllers:` identity accepts these named values, the
IO-Homecontrol alliance's own manufacturer IDs:

| Name | Hex ID | 1W profile | Name | Hex ID | 1W profile |
|---|---|---|---|---|---|
| `velux` | `0x01` | ✅ | `window_master` | `0x07` | — |
| `somfy` | `0x02` | ✅ | `renson` | `0x08` | — |
| `honeywell` | `0x03` | — | `ciat` | `0x09` | — |
| `hormann` | `0x04` | — | `secuyou` | `0x0A` | — |
| `assa_abloy` | `0x05` | — | `overkiz` | `0x0B` | — |
| `niko` | `0x06` | — | `atlantic_group` | `0x0C` | — |

A manufacturer not in this table can still be declared as a raw hex ID, e.g. `manufacturer: 0x0D`.

**1W profile** marks the two vendors this project has a verified 1W `CMD_EXECUTE` wire profile for
(the priority byte — `somfy` `0x43`, `velux` `0x61`). Any other value — named or raw — transmits
with the `somfy`-shaped byte and, if it is an explicitly-set manufacturer on a transmitting
identity, logs a build-time warning. Use `execute_acei:` to pin the byte yourself for an unprofiled
vendor.

## Device pages

Seven devices have enough evidence behind them to be worth a page of their own.

<!-- doxygen-subpages -->
- [Somfy Izymo IO dimmer](devices/somfy-izymo-dimmer.md)
- [Somfy Sunea IO](devices/somfy-sunea-io.md)
- [VELUX INTEGRA and the KLR/KLF/KUX family](devices/velux-integra.md)
- [Somfy RS100 IO](devices/somfy-rs100.md)
- [Somfy White LED Receiver io](devices/somfy-white-led-receiver.md)
- [SIMU T3.5 E BHz DC solar shutter motor](devices/simu-t35-bhz.md)
- [Atlantic AGILIA IO electric radiator](devices/atlantic-agilia-io.md)
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
