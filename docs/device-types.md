# Device types and manufacturers
<!-- doxygen-label: device_types -->

The names and numbers this component uses for IO-Homecontrol device types, their subtypes and the
manufacturers: what `io_device_type`, `io_subtype`, `io_model` and `manufacturer` accept, and what
each type becomes in Home Assistant. For which products have been run on real hardware, see
[Supported devices](supported-devices.md).

## Named device types

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
| `heating_temperature_interface` | `0x0E` | `domestic_hot_water` | `0x33` |
| `on_off_switch` | `0x0F` | | |

A device type not in this table can still be declared as a raw hex ID, in either place:
`io_device_type: 0x14` or `linked_remotes: ["class:0x14"]`. Named and raw-hex entries can be mixed
freely within the same `linked_remotes` list.

**Finding your device's type:** the surest way is to pair it through Home Assistant and check the
"Last Pairing Result" diagnostic sensor — its `type=` field reports the device's actual type name,
e.g. `type=vertical_exterior_awning`. If you already know what the device is (say, a Somfy awning), use the matching
name from the table above.

## Node types and subtypes

Discovery, Scan Paired Devices and the pairing log name a device as `<type>, <subtype>` followed by
the wire values, for example `electrical heater, with adjustable setpoint (0x34/1)`; a subtype
without a name prints as `subtype N`. A subtype the table does not list behaves like its type. Subtype 58 means "main parameter only" on
every type: such a device takes position commands without the functional-parameter block. The component also reads it as "no intermediate positions", which the VELUX specification does not say; it rests on the garage opener of issue #163. A gate opener of subtype 58 is reported to open, close and stop (issue #244); a swinging shutter of subtype 58 is experimental. A cover of such a kind shows open, close and stop, and no slider; see [Covers that only open and close](configuration/cover.md#covers-that-only-open-and-close).

A subtype can list the product models below it as `(io_model: name)`. Products of one model report the
same type and subtype as the rest but need different handling, and the radio does not say which one a
device is, so you declare it with [`io_model`](configuration/index.md#options-every-entity-shares).

Types marked "not supported yet" are recognised and named, but no platform drives them, so
discovery reports them and prints no YAML. Subtype names that have not been confirmed on a real
device are marked experimental; declaring one in YAML logs a warning when the config is built.

| Type | Name | Named subtypes | Support |
|---|---|---|---|
| `0x01` | interior venetian blind | | Cover with tilt; short position frame, no `silent:` or `travel_speed:` (experimental, [details](configuration/cover.md#interior-venetian-blinds-and-dual-roller-shutters)) |
| `0x02` | roller shutter | 1 with adjustable slats, 2 with projection | Cover |
| `0x03` | vertical exterior awning | | Cover |
| `0x04` | window opener | 1 with integrated rain sensor | Cover, with a ventilation position |
| `0x05` | garage opener | 58 open/close only | Cover: percent positions on subtype 0, open / close / stop only on subtype 58; no favorite position (issue #163); Partial Position button |
| `0x06` | light | 1 dimmable RGB (experimental), 2 tunable white (experimental), 58 on/off only | Light; subtypes 1 and 2 control brightness only (experimental) |
| `0x07` | gate opener | 58 open/close only | Cover; subtype 58 shows open / close / stop only (issue #244); Pedestrian Position button (experimental) |
| `0x08` | rolling door opener | | Cover |
| `0x09` | lock | 1 window lock (experimental), 2 car controller (experimental) | Lock (experimental); subtype 2 not supported yet |
| `0x0A` | vertical interior blind | | Cover without tilt (experimental) |
| `0x0B` | secure configuration device | | Not supported yet |
| `0x0C` | beacon | | Not supported yet |
| `0x0D` | dual roller shutter | | Cover; short position frame, no `silent:` or `travel_speed:` (experimental, [details](configuration/cover.md#interior-venetian-blinds-and-dual-roller-shutters)) |
| `0x0E` | heating temperature interface | | Climate with a setpoint, off / heat / auto and comfort / eco / secured presets (experimental, untested); see [Thermostats and heat pumps](heating.md#thermostats-and-heat-pumps) |
| `0x0F` | on off switch | 2 micromodule | Switch; a subtype 2 micromodule is reported working (issue #57) |
| `0x10` | horizontal awning | 1 rail-guided pergola | Cover, direction inverted |
| `0x11` | exterior venetian blind | | Cover with tilt |
| `0x12` | louvre blind | | Cover with tilt |
| `0x13` | curtain track | | Cover |
| `0x14` | ventilation point | 1 air inlet, 2 air transfer, 3 air outlet | Not supported yet |
| `0x15` | exterior heating | 58 on/off only | Fan, a heat level of 0–100 % (subtype 58: on/off), experimental and unconfirmed on hardware; see [Exterior heating](configuration/fan.md) |
| `0x16` | heat pump | 1 heat pump (Atlantic), 2 boiler (Atlantic) (experimental) | Climate for subtype 0 with a setpoint, off / heat and comfort / eco / setback / halted presets (experimental, untested); subtype 1 reads its heating and hot-water energy counters and its outdoor temperature through a `sensor:` entry, confirmed on one installation ([Atlantic Alfea heat pump](devices/atlantic-heat-pump.md), issue #244); subtype 2 is not supported yet |
| `0x17` | intrusion alarm | | Not supported yet |
| `0x18` | swinging shutter | 1 with independent leaves, 58 open/close only (experimental) | Cover; subtype 58 shows open / close / stop only (experimental) |
| `0x1B` | sliding window | 1 with lock (experimental) | Cover |
| `0x1C` | zone control | | Not supported yet |
| `0x1D` | bioclimatic pergola | | Cover |
| `0x1E` | siren | | Not supported yet |
| `0x33` | domestic hot water | 0 domestic hot water (io_model: electric_water_heater / explorer_v4) | Water heater, confirmed on an [Atlantic Calypso Connecté](devices/atlantic-calypso.md), an [Atlantic Explorer V4](devices/atlantic-explorer-v4.md) (`io_model: explorer_v4`) and a [Sauter Guelma connecté](devices/sauter-guelma.md) (`io_model: electric_water_heater`); see [Water heaters](configuration/water-heater.md) |
| `0x34` | electrical heater | 0 electrical heater (io_model: pilot_wire_interface), 1 with adjustable setpoint (io_model: heater_with_light / towel_dryer) | Climate. Subtype 1 is a radiator with a setpoint, a towel dryer (`io_model: towel_dryer`) or a heater with a night light (`io_model: heater_with_light`); subtype 0 is a level heater or a pilot-wire interface (`io_model: pilot_wire_interface`). See [What each heater kind offers](heating.md#what-each-heater-kind-offers) |
| `0x35` | heat recovery ventilation | | Not supported yet |
| `0xFF` | central house control | | Not supported yet |
| `0x3F0` | test and evaluation | | Not supported yet |
| `0x3FF` | remote controller | 2 Keygo (experimental), 3 Izymo (experimental), 4 public frame (experimental), 5 V500 (experimental) | Not supported yet; see [pairing](pairing.md) for devices that answer discovery as a remote controller |

## Named manufacturers

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

## See also

- [Supported devices](supported-devices.md) — which products have been run, and how
- [Configuration reference](configuration/index.md) — the keys that take these names
