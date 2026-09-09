# Configuration reference
<!-- doxygen-label: cfg_index -->

Every YAML key this component accepts, grouped by what it configures.

## How these pages are organised

The hub comes first: one `home_io_control:` block per board, holding the radio pins and the
installation's identity. Everything else attaches to it.

| Page | What it configures |
|---|---|
| This page | The `home_io_control:` hub block, and the options every entity shares |
| [Covers](cover.md) | `cover:` — shutters, blinds, awnings, openers, curtains |
| [Lights, locks and switches](light-lock-switch.md) | `light:`, `lock:`, `switch:` |
| [Heating and climate](climate.md) | `climate:` — experimental |
| [Home Assistant actions](actions.md) | Hub-level actions such as `rename_device` and `scan_paired_devices` |
| [Linked remotes and sender events](remotes.md) | `linked_remotes:`, `exposed_senders:` |
| [Sending 1W commands](oneway-transmit.md) | `oneway_controllers:` — driving devices that only listen for a remote |
| [Radio tuning](tuning.md) | `tuning:` — radio timing and discovery parameters |

## The `home_io_control:` hub block

One per board. It owns the radio and the installation's identity, and every cover, light, lock,
switch and button entity attaches to it.

The pins below are for a Heltec WiFi LoRa 32 (V3) and its SX1262 — take the ones for your own board
from [Hardware](../hardware.md#all-supported-boards). Which pin keys you need depends on the radio
chip rather than the board; see [Radio pin requirements](../hardware.md#radio-pin-requirements).

<!-- board-pinout: heltec-v3 -->
```yaml
home_io_control:
  cs_pin: 8
  rst_pin: 12
  dio1_pin: 14
  busy_pin: 13
  radio_type: sx1262
  tcxo_voltage: 1_8V
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"
```

## Configuration variables:

- `id` (Optional): Manually specify the hub ID for code generation. Use this when entity blocks should reference a specific hub with `home_io_control_id`.
- `cs_pin` (Required): SPI chip select pin for the radio.
- `rst_pin` (Required): Radio reset pin.
- `dio0_pin` (Optional): SX1276 DIO0 interrupt pin.
- `dio4_pin` (Optional): SX1276 DIO4 preamble-detect pin. Most boards do not wire this.
- `dio1_pin` (Optional): The chip's IRQ line — SX1262 DIO1, or LR1121 DIO9.
- `busy_pin` (Optional): SX1262/LR1121 BUSY pin.
- `node_id` (Required): 3-byte controller ID as exactly 6 hexadecimal characters.
- `system_key` (Required): 16-byte installation key as exactly 32 hexadecimal characters.
- `tx_power` (Optional, default: `17`): Radio transmit power, `0` to `22`.
- `pa_pin` (Optional, default: `BOOST`): SX1276 PA path. Valid values are `BOOST` and `RFO`.
- `radio_type` (Required): The radio chip fitted to your board. Valid values are `sx1276`, `sx1262`, and `lr1121` — see [Hardware](../hardware.md).
- `fem_en_pin` (Optional): Front-end module enable pin for boards with an external RF front-end.
- `vfem_pin` (Optional): Front-end module power pin for boards with an external RF front-end.
- `fem_pa_pin` (Optional): Front-end module PA select pin for boards with an external RF front-end.
- `tcxo_voltage` (Optional, default: `1_8V`): SX1262/LR1121 TCXO voltage. Valid values are `1_6V`, `1_7V`, `1_8V`, `2_2V`, `2_4V`, `2_7V`, `3_0V`, and `3_3V`.
- `exposed_senders` (Optional, default: empty list): List of 1W sender node IDs (6 hex characters each — remotes *or* sensors) allowed to fire the `esphome.home_io_control_sender_event` event to Home Assistant. Empty by default — see [Why this is opt-in](remotes.md#why-this-is-opt-in), and
  [Linked remotes](remotes.md#linked-remotes) for how this differs from `linked_remotes`.
- `tuning` (Optional): Diagnostics block for pairing/radio parameters. See [Radio tuning](tuning.md).
- `accept_foreign_pairing` (Optional, default: `false`): Adds a "Recover System Key" switch entity for pulling a device's system key from another controller. See [Key extraction](../key-extraction.md).
- `scan_paired_devices_button` (Optional, default: `false`): Adds a "Scan Paired Devices" button entity — a one-tap trigger for the `scan_paired_devices` action. See
  [Scan Paired Devices](../pairing.md#scan-paired-devices).
- `diagnostic_probes` (Optional, default: `false`): Enables the `probe_device`/`probe_sweep` actions for sending opcodes this project hasn't fully decoded yet. See
  [Diagnostic probes](../diagnostic-probes.md#calling-the-actions).

## Options every entity shares

`cover:`, `light:`, `lock:` and `switch:` all bind an entity to one device on the hub, so they take
the same set of keys. Platform-specific options are documented on each platform's own page.

- `name` (Required): The entity's name in Home Assistant. ESPHome's device-name idiom (`name: ""`,
  or the literal `name: None`) is supported, but an entity using it must also declare an explicit
  `id:` — otherwise the companion entity IDs below cannot be derived uniquely.
- `home_io_control_id` (Optional): Which hub this entity attaches to. Only needed when more than
  one `home_io_control:` block exists.
- `io_device_id` (Required): The device's 3-byte address, as exactly 6 hexadecimal characters. If
  you do not know it yet, use the Discover & Pair button — see [Pairing](../pairing.md).
- `io_device_type` (Optional): The device's IO-Homecontrol type, named (`awning`) or raw (`0x11`).
  Declaring it is what enables type-specific behaviour such as the Favorite Position button and
  automatic direction inversion. The full list of names is in
  [Supported devices](../supported-devices.md). A declared type is authoritative: the hub may
  learn a type from the radio when none is declared, but never overwrites a declared one.
- `io_subtype` (Optional): The device's subtype, `0`–`63`.
- `status_poll_interval` (Optional): Follow-up poll cadence while movement is expected. See the
  note below.
- `low_power` (Optional): Marks a battery or solar device, which changes how the hub addresses it
  on the radio. Set this on solar and battery actuators; leaving it off on such a device is a
  common cause of a hub that pairs but then hears nothing back.
- `linked_remotes` (Optional): 1W remotes or device classes whose presses should update this
  entity's state. See [Linked remotes and sender events](remotes.md).

### Companion entities

Every entity on these platforms generates seven companion diagnostic entities named after the
parent: Active Issue (enabled by default), Device Name, RSSI, Last Contact, Exchange Failures,
Last Commanded By and Last Command Source.
[Diagnostic entities](../diagnostic-entities.md) explains each one and when to enable it.

## Notes

- The SPI bus itself is configured separately in the top-level `spi:` block.
- The component extends ESPHome's SPI device schema, so standard SPI-device options apply in addition to the keys above.
- Some boards route the SPI bus through an ESP32 strapping pin (GPIO5's `clk_pin` on classic ESP32, GPIO3's `miso_pin` on ESP32-S3). ESPHome refuses to reuse a strapping pin as a plain GPIO unless you set `ignore_strapping_warning: true` on that pin's expanded schema, e.g. `clk_pin: {number: 5, ignore_strapping_warning: true}`. The Heltec V3 used in this page's examples needs none of this; see [Hardware](../hardware.md#all-supported-boards) for the boards that do.
- SX1276 uses a different interrupt pin set (`dio0_pin`/`dio4_pin`) than SX1262/LR1121 (`dio1_pin`/`busy_pin`, shared — LR1121 reuses the same two keys, with `dio1_pin` carrying its DIO9 line). Only configure the pins for the radio you actually have.
- User control commands (position, STOP, tilt, light/switch/lock state) are prioritized over background status polls in the operation queue. A queued status poll for the same device is dropped when a control command arrives, since the command reply provides fresher state. An in-flight exchange cannot be interrupted, so the worst-case latency is one full exchange (~1–3 s) regardless of the queue.
- If a device does not emit unsolicited status updates on its own, set `status_poll_interval` on the affected entry. Without it, the hub polls after commands and overheard remote activity at the device-reported settle hint (fallback 3 s) until the device reports a stable state or the bounded 10-minute window expires. With it, the hub uses min(device hint, configured interval) — the device can shorten your interval but never stretch it. A STOP command always confirms the resting position within ~1 s regardless. Either way polling stops once the device settles. The minimum is `500ms`.

## A complete example

One worked config, for a Heltec WiFi LoRa 32 (V3) — an SX1262 board. The pin block is the only
part that differs between boards — take the pins for yours from
[Hardware](../hardware.md#all-supported-boards).

<!-- board-pinout: heltec-v3 -->
```yaml
esphome:
  name: io-homecontrol-sx1262

esp32:
  variant: esp32s3
  framework:
    type: esp-idf

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

logger:

api:

ota:
  - platform: esphome

spi:
  clk_pin: 9
  mosi_pin: 10
  miso_pin: 11

external_components:
  - source: github://laberning/home_io_control

home_io_control:
  cs_pin: 8
  rst_pin: 12
  dio1_pin: 14
  busy_pin: 13
  radio_type: sx1262
  tcxo_voltage: 1_8V
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"

cover:
  - platform: home_io_control
    name: "Awning"
    device_class: awning
    io_device_id: "FEEB1E"
    io_device_type: "awning"
    io_subtype: 0
    invert_position: true

button:
  - platform: home_io_control
    name: "Discover & Pair"
```

With `io_device_type: "awning"`, the example above also generates an `Awning Favorite Position`
button automatically.

## Working configs in this repo

These are compiled in CI on every commit, so they are always valid. For an SX1276 or LR1121 board,
start from the matching one rather than adapting the example above by hand.

| Config | Board and radio | What it contains |
|---|---|---|
| [heltec-wifi-lora-32-v2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2.yaml) | Heltec LoRa32 V2, SX1276 | One awning cover, Discover & Pair, and an OLED status display |
| [heltec-wifi-lora-32-v3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v3.yaml) | Heltec WiFi LoRa32 V3/V3.2, SX1262 | The same, with the V3 pinout and TCXO settings |
| [t3s3-lr1121.yaml](https://github.com/laberning/home_io_control/blob/main/config/t3s3-lr1121.yaml) | LilyGO T3-S3, LR1121 | The same, for the LR1121 |
| [heltec-wifi-lora-32-v2-all-types.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2-all-types.yaml) | Heltec LoRa32 V2, SX1276 | Every supported platform — cover, light, lock, switch, button — with dummy device IDs ready to replace |
| [heltec-wifi-lora-32-v3-monitor.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v3-monitor.yaml) | Heltec WiFi LoRa32 V3/V3.2, SX1262 | A passive monitor: keeps the radio in RX, enables `IOHOME_FRAME_LOG`, creates no entities and no pairing button |

**These files are not standalone.** Each pulls its board's SPI bus and radio pin assignment from a
package:

```yaml
packages:
  board: !include boards/heltec-v3.yaml
```

The per-board pinouts live once in
[config/boards/](https://github.com/laberning/home_io_control/tree/main/config/boards) —
`heltec-v2.yaml`, `heltec-v3.yaml`, `t3s3.yaml`. To reuse one of the configs above, copy the whole
`config/` directory (or at least the matching `config/boards/*.yaml` alongside the file you took),
or replace the `packages:` line with the inline pin values from the worked example above.

<!-- doxygen-subpages -->
- [Covers](cover.md)
- [Lights, locks and switches](light-lock-switch.md)
- [Heating and climate](climate.md)
- [Home Assistant actions](actions.md)
- [Linked remotes and sender events](remotes.md)
- [Sending 1W commands](oneway-transmit.md)
- [Radio tuning](tuning.md)
<!-- /doxygen-subpages -->
