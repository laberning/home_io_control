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
strongest kind of evidence here. Issue numbers refer to
[GitHub issues](https://github.com/laberning/home_io_control/issues).

A device's absence is not a verdict. The protocol is the same across the alliance's members, so an
unlisted cover-like actuator has a good chance of working with `io_device_type` set to the closest
match.

## Device matrix

### Somfy — actuators

| Device | Type | Status | Route that worked | Key config | Evidence |
|---|---|---|---|---|---|
| **Sunea IO awning motor** | `awning` | ✅ Confirmed | Discover & Pair | `io_device_type: awning` | 31 corpus captures, across all three radios |
| **Sunea IO 70/17** | `roller_shutter` | ✅ Confirmed | Discover & Pair | — | Issue #65 |
| **Sunea IO 40/17** | `roller_shutter` | ✅ Confirmed | Discover & Pair | — | Issue #65 |
| **Sunilus IO 50/12** | `roller_shutter` | ✅ Confirmed | Discover & Pair | — | Issue #65; corpus `somfy_sunilus_pairing_key_transfer_rejected_error` |
| **Izymo IO dimmer** | `light` | ✅ Confirmed, including dimming | Discover & Pair, including repeated reset-and-repair cycles | `dimmable: true` | 39 corpus captures; the `light:` platform's hardware validation |
| **J406 IO shutter motor** | `roller_shutter` | ✅ Confirmed | Discover & Pair | — | Corpus `somfy_j406_discovery_1w_overheard` |
| **MAESTRIA+ IO** | `roller_shutter` | ✅ Confirmed | **Key extraction** — Discover & Pair did not succeed | `accept_foreign_pairing: true` | Issue #103, on a Heltec V3.2 / SX1262 |
| **Horizontal awning** (two units) | `horizontal_awning` | ✅ Confirmed | Discover & Pair | Direction inversion is applied automatically for this family | Corpus `somfy_awning_discovery_spe_paired_rollcall` |
| **Awning actuator (IO Vertical)** | `awning` | ✅ Confirmed for discovery | Discover & Pair | — | Corpus `somfy_awning_discovery_lab_response` |
| **RS100 IO / RS100 Solar** | `awning` / `roller_shutter` | ⚠️ Partial — pairing needs a retried key exchange | Discover & Pair, retried | `low_power: true` | Issues #16, #45; 7 corpus captures including both a success and a key-transfer stall |
| **Oximo 40 Solar tubular motor** | `roller_shutter` | 🔍 Traffic captured only | — | `low_power: true` | Corpus `somfy_oximo40_statuspoll_sx1262`, issue #45 |
| **Sunea IO screen** | `screen` | 🔍 Traffic captured only | — | — | Corpus `somfy_awning_exchange_set_sensor_sx1276` — a directed write from a real TaHoma Switch |
| **Tilt-capable blind/shutter** | tilt-capable cover | ✅ Confirmed for tilt control | — | — | 8 corpus captures |
| **Combined wind/rain protection station** | sensor (1W) | ✅ Confirmed for listening | Overheard; no pairing involved | `exposed_senders:` / `linked_remotes:` | 2 corpus captures |

`low_power: true` on the solar Somfy motors is a reasoned starting point rather than a confirmed
setting: the option is meant for battery and solar actuators, and it is confirmed on VELUX hardware
through issue #87, but no RS100 or Oximo report has confirmed it yet.

### Somfy — hubs and remotes

These are useful as key-extraction sources or as senders you can listen to, not as things this
project controls.

| Device | Role | Status | Evidence |
|---|---|---|---|
| **Smoove IO wall switch** | 1W remote | ✅ Confirmed for listening, and for re-pairing a reset Izymo | 8 corpus captures |
| **TaHoma Switch** | Third-party hub | ✅ Confirmed as a key-extraction source | Corpus `somfy_tahoma_pairing_key_extraction_success_sx1276`; issue #27 |
| **Connectivity Kit** | Third-party hub | ⚠️ Partial — extraction stalled | Corpus `somfy_connectivity_kit_pairing_key_extraction_stall` |
| **Smoove IO remote** | 1W remote | ✅ Confirmed for listening | Corpus `somfy_smoove_enrollment_monitor_sx1276`; issue #27 |

### VELUX

| Device | Type | Status | Route that worked | Key config | Evidence |
|---|---|---|---|---|---|
| **KLR 200 two-way control pad** | Third-party hub | ✅ Confirmed as a key-extraction source | Key extraction | `accept_foreign_pairing: true` | Issue #80 — succeeded end to end on the first attempt, including the address round; corpus `velux_klr200_pairing_key_extraction_success` |
| **KIG 300 hub** | Third-party hub | ⚠️ Partial — one extraction succeeded, one stalled | Key extraction | `accept_foreign_pairing: true` | 4 corpus captures |
| **INTEGRA roof-window actuator** | `window_opener` | ✅ Confirmed | — | `io_device_type: window_opener`, which adds the Ventilation Position button | 3 corpus captures; issue #98 |
| **Devices behind an extracted KLR200 key** | Mixed | ✅ Confirmed | Key extraction, then `scan_paired_devices` | **`low_power: true`** on the affected devices | Issue #87 — the roll-call drew no replies until the devices were declared `low_power: true` |
| **MSU 100100 5070WL solar awning screen** | `screen` | ⚠️ Partial — open and close work, `stop` is ignored | — | — | Issue #95 — the screen will not complete a 2W handshake mid-motion, so `cover.stop` has nothing to talk to |
| **INTEGRA SOLAR blinds** | `blind` | 📣 Reported | — | `low_power: true` | Named as a canonical low-power case; no capture |
| **KLI 313 remote** (driving an SML via KUX 110) | 1W remote | 🔍 Traffic captured only | — | — | 3 corpus captures; issue #95 |
| **KLI 310 universal wall remote** | 1W remote | 🔍 Traffic captured only | — | — | Corpus `velux_kli310_discovery_alt_sweep` |
| **KUX 100 / PK03 / PK04 wired bridge** | Bridge | 🔍 Traffic captured only | — | — | 3 corpus captures of KLR200 ↔ KUX100 traffic |
| **KLF 200** | Third-party hub | 📣 Reported as a key source | — | — | Named in several issue threads; no capture |
| **SML shutter via KUX 110** | `roller_shutter` | ❌ Unsupported — nothing answers discovery | — | — | Issue #17 |
| **KUX 110 / KUX 100 wired bridge, 1W enrollment** | — | ❌ Unsupported — enrollment transmits, the device never reacts | Tried with the `roller_shutter` and `dual_shutter` classes | `oneway_controllers:` with `enrollment: true` | Issue #74. The `awning` class has not been tried yet. |

### Other vendors

| Device | Status | Evidence |
|---|---|---|
| **Atlantic Thermor-style climate/heating actuator** | 🔍 Traffic captured only — the `climate:` platform is built against it but has never run on hardware | Corpus `atlantic_thermor_exchange_write_private_param` |
| **Honeywell, Hörmann, Assa Abloy, Niko, WindowMaster, Renson, CIAT, Secuyou, Overkiz, Atlantic Group** | Untested — these are names the schema accepts, not devices anyone has reported | Only `somfy` and `velux` have a verified 1W wire profile |

## Naming reference

### Named device types

Both `io_device_type` and the `class:<device_type>` form of `linked_remotes` accept these named
values:

| Name | Hex ID | Name | Hex ID |
|---|---|---|---|
| `venetian_blind` | `0x01` | `on_off_switch` | `0x0F` |
| `roller_shutter` | `0x02` | `horizontal_awning` | `0x10` |
| `awning` | `0x03` | `external_venetian_blind` | `0x11` |
| `window_opener` | `0x04` | `louvre_blind` | `0x12` |
| `garage_opener` | `0x05` | `curtain_track` | `0x13` |
| `light` | `0x06` | `intrusion_alarm` | `0x17` |
| `gate_opener` | `0x07` | `swinging_shutter` | `0x18` |
| `rolling_door_opener` | `0x08` | | |
| `lock` | `0x09` | | |
| `blind` | `0x0A` | | |
| `screen` | `0x0B` | | |
| `dual_shutter` | `0x0D` | | |
| `heating_temperature_interface` | `0x0E` | | |

A device type not in this table can still be declared as a raw hex ID, in either place:
`io_device_type: 0x14` or `linked_remotes: ["class:0x14"]`. Named and raw-hex entries can be mixed
freely within the same `linked_remotes` list.

**Finding your device's type:** the surest way is to pair it through Home Assistant and check the
"Last Pairing Result" diagnostic sensor — its `type=` field reports the device's actual type name,
e.g. `type=awning`. If you already know what the device is (say, a Somfy awning), use the matching
name from the table above.

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

Four devices have enough evidence behind them to be worth a page of their own.

<!-- doxygen-subpages -->
- [Somfy Izymo IO dimmer](devices/somfy-izymo-dimmer.md)
- [Somfy Sunea IO](devices/somfy-sunea-io.md)
- [VELUX INTEGRA and the KLR/KLF/KUX family](devices/velux-integra.md)
- [Somfy RS100 IO](devices/somfy-rs100.md)
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
