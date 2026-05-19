# YAML Configuration

This page documents the YAML configuration for the `home_io_control` external component and its ESPHome platforms.

## How It Works

IO-Homecontrol is a proprietary **868 MHz radio protocol** used by Somfy, Velux, and other manufacturers for motorized shutters, blinds, awnings, and related home devices. This component implements the **2-Way (2W)** variant, which means the controller sends commands and devices reply with position feedback.

Key concepts:

- **Node ID**: Every participant on the radio network has a unique 3-byte address (e.g., `C0FFEE`). You choose one for your controller and each device has one factory-assigned.
- **System key**: A 16-byte AES key shared between the controller and all paired devices. Commands that change device state (open, close, set position) are authenticated with this key using a challenge-response exchange.
- **Frequency hopping**: The controller hops between three 868 MHz channels (~2.7 ms per channel) while idle, listening for incoming status updates.
- **Pairing**: Before a device can be controlled, it must be paired — the controller transmits the system key to the device over a short encrypted exchange. The device must be in pairing mode (PROG button) during this step.
- **YAML as source of truth**: Device type and subtype live in the YAML config. Cover inversion can be forced with `invert_position`, otherwise the controller falls back to the learned device profile. Pairing prints a ready-to-paste YAML snippet in the logs when enough metadata is known.
- **Automatic status polling**: The controller periodically polls each device for its current position. Devices can also push unsolicited status updates, which the controller authenticates and processes automatically.

## Minimal Example

```yaml
external_components:
  - source: github://laberning/home_io_control

spi:
  clk_pin: 5
  mosi_pin: 27
  miso_pin: 19

home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"

cover:
  - platform: home_io_control
    name: "Patio Awning"
    device_class: awning
    io_device_id: "FEEB1E"

button:
  - platform: home_io_control
    name: "Discover & Pair"
```

## Home IO Control Component

The `home_io_control:` block defines the shared radio/controller hub. All cover, light, switch, and button entities attach to this hub.

```yaml
home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"
```

Configuration variables:

- `id` (Optional): Manually specify the hub ID for code generation. Use this when entity blocks should reference a specific hub with `home_io_control_id`.
- `cs_pin` (Required): SPI chip select pin for the radio.
- `rst_pin` (Required): Radio reset pin.
- `dio0_pin` (Optional): SX1276 DIO0 interrupt pin.
- `dio4_pin` (Optional): SX1276 DIO4 preamble-detect pin. Most boards do not wire this.
- `dio1_pin` (Optional): SX1262 DIO1 interrupt pin.
- `busy_pin` (Optional): SX1262 BUSY pin.
- `node_id` (Required): 3-byte controller ID as exactly 6 hexadecimal characters.
- `system_key` (Required): 16-byte installation key as exactly 32 hexadecimal characters.
- `tx_power` (Optional, default: `17`): Radio transmit power. The schema currently accepts `0` to `22`.
- `pa_pin` (Optional, default: `BOOST`): SX1276 PA path. Valid values are `BOOST` and `RFO`.
- `radio_type` (Optional): Force radio selection. Valid values are `sx1276` and `sx1262`. If omitted, the component auto-detects.
- `fem_en_pin` (Optional): Front-end module enable pin for boards with an external RF front-end.
- `vfem_pin` (Optional): Front-end module power pin for boards with an external RF front-end.
- `fem_pa_pin` (Optional): Front-end module PA select pin for boards with an external RF front-end.
- `tcxo_voltage` (Optional, default: `1_8V`): SX1262 TCXO voltage. Valid values are `1_6V`, `1_7V`, `1_8V`, `2_2V`, `2_4V`, `2_7V`, `3_0V`, and `3_3V`.

Notes:

- The SPI bus itself is configured separately in the top-level `spi:` block.
- The component extends ESPHome's SPI device schema, so standard SPI-device options apply in addition to the keys above.
- SX1276 and SX1262 use different interrupt pin sets. Only configure the pins for the radio you actually have.

### Radio-Specific Pin Requirements

| Radio | Required hub pins | Optional hub pins | Typical extra setting |
| --- | --- | --- | --- |
| SX1276 | `cs_pin`, `rst_pin`, `dio0_pin` | `dio4_pin` | `pa_pin: BOOST` |
| SX1262 | `cs_pin`, `rst_pin`, `dio1_pin`, `busy_pin` | `fem_en_pin`, `vfem_pin`, `fem_pa_pin` | `radio_type: sx1262`, `tcxo_voltage: 1_8V` |

## Cover Platform

Use the cover platform for position-capable IO-homecontrol devices such as shutters, awnings, blinds, openers, curtains, and related families.

```yaml
cover:
  - platform: home_io_control
    id: patio_awning
    name: "Patio Awning"
    device_class: awning
    io_device_id: "FEEB1E"
    invert_position: true
    status_poll_interval: 2s
    linked_remotes:
      - "ABCDEF"
```

Configuration variables:

- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use a named value such as `awning` when available, or a raw integer such as `0x11` if pairing reports a type that does not yet have a named YAML alias. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `invert_position` (Optional): Explicitly override the open/close position mapping. When omitted, the controller uses the learned device profile and automatically inverts families such as horizontal awnings once their type is known.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the controller keeps the legacy single follow-up settle poll after local commands or overheard remote activity, but does not continue repeated movement polling.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) that control this device. When activity from a linked remote is overheard on the radio, the controller automatically polls the device for fresh status 2 seconds later. This is particularly useful for 1W (one-way) remotes whose radio address differs from the device's 2W ID.
- All standard options from the ESPHome cover base schema also apply, including `id`, `name`, `device_class`, `icon`, entity metadata, MQTT options, and cover automations such as `on_opening`, `on_closing`, and `on_idle`.

Notes:

- This is the primary and best-validated platform in the repo.
- Additional recognized cover families include venetian blinds, dual shutters, louvre blinds, rolling door openers, curtain tracks, and swinging shutters.
- `status_poll_interval` is movement-scoped, not a continuous background refresh. The hub only keeps polling while a local command or overheard remote activity suggests that the device should still be changing, and it stops automatically once the device reports a stable state or the bounded polling window expires.
- Unsolicited `0x71` device status updates are always applied to the entity state. They only extend automatic repeated polling when `status_poll_interval` is configured; without it, the hub falls back to the single settle poll armed by the original command or remote activity.

## Light Platform

Use the light platform for binary on/off IO-homecontrol light devices.

```yaml
light:
  - platform: home_io_control
    id: garden_light
    name: "Garden Light"
    io_device_id: "D15C05"
```

Configuration variables:

- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use the named value `light` when known, or a raw integer such as `0x06` if you are working from a pairing log that reports a not-yet-exposed alias. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the controller keeps the default single settle poll after a local command or overheard remote activity.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) that control this device. See the cover platform for details.
- All standard options from the ESPHome light schema also apply.

Notes:

- This platform is intentionally binary only. Dimming is not exposed.
- The current implementation is still experimental and untested on local hardware in this repo.
- Known non-light device families will be rejected once the device type is known.

## Switch Platform

Use the switch platform for binary on/off IO-homecontrol switch devices.

```yaml
switch:
  - platform: home_io_control
    id: irrigation_switch
    name: "Irrigation Switch"
    io_device_id: "112233"
```

Configuration variables:

- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use the named value `on_off_switch` when known, or a raw integer such as `0x0F` if pairing reports a type without a named YAML alias yet. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the controller keeps the default single settle poll after a local command or overheard remote activity.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) that control this device. See the cover platform for details.
- All standard options from the ESPHome switch schema also apply.

Notes:

- This platform is also experimental and currently limited to binary on/off semantics.
- Known non-switch device families will be rejected once the device type is known.

## Button Platform

Use the button platform to expose a Home Assistant button that starts discovery and pairing.

```yaml
button:
  - platform: home_io_control
    name: "Discover & Pair"
```

Configuration variables:

- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- All standard options from the ESPHome button schema also apply.

Notes:

- The generated button defaults to the `config` entity category.
- Pair devices one at a time.

## Complete Examples

### Minimal SX1276 Cover Controller

```yaml
esphome:
  name: io-homecontrol-sx1276

esp32:
  variant: esp32

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

logger:

api:

ota:
  - platform: esphome

spi:
  clk_pin: 5
  mosi_pin: 27
  miso_pin: 19

external_components:
  - source: github://laberning/home_io_control

home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
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

### Minimal SX1262 Cover Controller

```yaml
esphome:
  name: io-homecontrol-sx1262

esp32:
  variant: esp32s3

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

### Minimal Example with all Device Types: Cover, Light, and Switch

```yaml
home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"

cover:
  - platform: home_io_control
    id: patio_awning
    name: "Patio Awning"
    device_class: awning
    io_device_id: "123ABC"
    io_device_type: "awning"
    io_subtype: 0

light:
  - platform: home_io_control
    id: garden_light
    name: "Garden Light"
    io_device_id: "D15C05"
    io_device_type: "light"
    io_subtype: 0

switch:
  - platform: home_io_control
    id: irrigation_switch
    name: "Irrigation Switch"
    io_device_id: "D0661E"
    io_device_type: "on_off_switch"
    io_subtype: 0

button:
  - platform: home_io_control
    name: "Discover & Pair"
```

### Repo-Backed Example Configs

For larger working examples, see the configs already in this repo:

- [config/heltec-wifi-lora-32-v2.yaml](../config/heltec-wifi-lora-32-v2.yaml): SX1276 Heltec LoRa32 V2 controller config with one awning cover, a Discover & Pair button, and an OLED status display that shows recent activity.
- [config/heltec-wifi-lora-32-v2-all-types.yaml](../config/heltec-wifi-lora-32-v2-all-types.yaml): SX1276 Heltec LoRa32 V2 controller config without OLED support that exercises every currently supported ESPHome platform in this component: cover, light, switch, and the Discover & Pair button, all with dummy device IDs ready to replace.
- [config/heltec-wifi-lora-32-v3.yaml](../config/heltec-wifi-lora-32-v3.yaml): SX1262 Heltec WiFi LoRa32 V3/V3.2 controller config with one awning cover, a Discover & Pair button, and an OLED status display tuned for the V3 pinout and TCXO settings.
- [config/heltec-wifi-lora-32-v3-monitor.yaml](../config/heltec-wifi-lora-32-v3-monitor.yaml): SX1262 passive monitor config for Heltec WiFi LoRa32 V3/V3.2 that keeps the radio in RX, enables `IOHOME_FRAME_LOG`, and logs parsed traffic without creating entities or exposing a pairing button.

## Pairing Workflow

1. Choose a controller `node_id` (any unique 6-character hex string) and `system_key` (32-character hex string). Keep these consistent across firmware updates.
2. Flash the firmware with at least the `home_io_control:` hub and a `button:` entity configured.
3. Put exactly **one** target device into pairing mode by pressing its PROG button. The pairing window is short — typically a few seconds.
4. Press the **Discover & Pair** button entity in Home Assistant within that window.
5. Watch the ESPHome logs. On success you will either get a ready-to-paste YAML snippet with `io_device_id`, `io_device_type`, and `io_subtype`, or a follow-up message explaining why a snippet could not be generated.
6. Copy `io_device_type` and `io_subtype` from the log snippet and add them, together with `io_device_id`, to the appropriate `cover:`, `light:`, or `switch:` entry in your YAML. If the log uses a raw numeric type such as `0x11`, keep that exact value in YAML.
7. Reflash with the updated YAML. The entity will appear in Home Assistant and the controller will begin polling the device for status.
8. If pairing reports that the type is unsupported or that discovery metadata was incomplete, follow the log guidance and please file a GitHub issue with the raw type/subtype, device model, and pairing log so support can be added.

## Device Type and Capability Notes

- **Cover-like families** (shutters, awnings, blinds, openers, curtains) are the primary supported path today. These support full position control (0–100%).
- **Binary light and switch** support exists, but remains experimental and has not been validated against real hardware.
- **Raw type IDs in YAML**: `io_device_type` accepts both named values such as `awning` and raw integers such as `0x11`. Raw values are useful when pairing discovers a valid IO-homecontrol type that this project does not yet expose under a named YAML alias.
- **Device type learning**: The YAML-declared `io_device_type` is the permanent, authoritative type. The controller may still learn a device's type from radio for runtime profile selection when the type is not declared in YAML, but it will never overwrite a YAML-declared type.
- **Inversion defaults**: Some device families (e.g., horizontal awnings) default to inverted position mapping. When `invert_position` is omitted, the cover entity follows that learned device profile automatically. Setting `invert_position` explicitly overrides the learned value.
- Additional reference-derived device types such as locks, heating devices, sensors, and beacons are recognized for classification and logging, but they do not yet have dedicated ESPHome platform support.

## See Also

- [ESPHome External Components](https://esphome.io/components/external_components/)
- [ESPHome Cover Component](https://esphome.io/components/cover/)
- [ESPHome Light Component](https://esphome.io/components/light/)
- [ESPHome Switch Component](https://esphome.io/components/switch/)
- [ESPHome Button Component](https://esphome.io/components/button/)