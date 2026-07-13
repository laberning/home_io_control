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
- **Automatic status polling**: After a command (or overheard remote activity) the controller polls the affected device until it reports a stable position, within a bounded window; an optional per-entity `status_poll_interval` sets the follow-up cadence. Devices can also push unsolicited status updates, which the controller authenticates and processes automatically.

## Minimal Example

```yaml
api:

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

When `api:` is enabled, Home IO Control also exposes hub-level Home Assistant actions. The component currently exposes one such action: device rename.

## Home IO Control Component

The `home_io_control:` block defines the shared radio/controller hub. All cover, light, lock, switch, and button entities attach to this hub.

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
- `exposed_senders` (Optional, default: empty list): List of 1W sender node IDs (6 hex characters each — remotes *or* sensors, see below) allowed to fire the `esphome.home_io_control_sender_event` event to Home Assistant. Empty by default — see the Sender Events section below for why this is opt-in and how it relates to `linked_remotes`.
- `tuning` (Optional): Diagnostics block for pairing/radio parameters. See [Radio Diagnostics Tuning](radio_diagnostics.md).

Notes:

- The SPI bus itself is configured separately in the top-level `spi:` block.
- The component extends ESPHome's SPI device schema, so standard SPI-device options apply in addition to the keys above.
- SX1276 and SX1262 use different interrupt pin sets. Only configure the pins for the radio you actually have.
- User control commands (position, STOP, tilt, light/switch/lock state) are prioritized over background status polls in the operation queue. A queued status poll for the same device is dropped when a control command arrives, since the command reply provides fresher state. An in-flight exchange cannot be interrupted, so the worst-case latency is one full exchange (~1–3 s) regardless of the queue.

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
    io_device_type: "awning"
    invert_position: true
    status_poll_interval: 2s
    linked_remotes:
      - "ABCDEF"
      - "class:awning"
    optimistic_state: true
```

Configuration variables:

- `name` (Required): The entity name as shown in Home Assistant. Without a name the entity would be invisible to HA, so this field is enforced at compile time.
- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use a named value such as `awning` when available — see the "Named device types" table under Device Type and Capability Notes below for the full list and how to find your device's type — or a raw integer such as `0x11` if pairing reports a type that does not yet have a named YAML alias. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `invert_position` (Optional): Explicitly override the open/close position mapping. When omitted, the controller uses the learned device profile and automatically inverts families such as horizontal awnings once their type is known.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the hub polls after commands, STOP, or overheard remote activity at the **device-reported settle hint** (fallback 3 s) until the device reports a stable state or the bounded 10-minute window expires. No idle background polling either way. See the settle-hint note below.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) — or `class:<device_type>` entries (e.g. `class:awning`) to link every device of that type at once — that control this device. When activity from a linked remote is overheard on the radio, the controller automatically polls the device for fresh status. See the Linked Remotes section below for how to find and configure remote IDs, and for the `class:` form.
- `optimistic_state` (Optional, default: `true`): Show the requested position/movement direction immediately in Home Assistant — for both HA-issued commands (open/close/set-position/stop) and commands from a linked 1W remote — instead of waiting for the confirming poll or device response. The confirming poll/response always still runs and is the source of truth; optimistic state is a UX bridge only. Set `false` to disable optimistic state for a device where you don't want HA to show assumed movement (e.g. one with an unreliable RF link where a stale optimistic state could be misleading).
- All standard options from the ESPHome cover base schema also apply, including `id`, `device_class`, `icon`, entity metadata, MQTT options, and cover automations such as `on_opening`, `on_closing`, and `on_idle`.

Notes:

- This is the primary and best-validated platform in the repo.
- Additional recognized cover families include venetian blinds, dual shutters, louvre blinds, rolling door openers, curtain tracks, and swinging shutters.
- **Tilt (slat angle) support**: When `io_device_type` is declared as a tilt-capable type (`venetian_blind`, `external_venetian_blind`, `blind`, or `louvre_blind`), the cover entity automatically exposes a tilt slider in Home Assistant. Tilt-only commands keep the current position unchanged. When Home Assistant sends both a position and a tilt value simultaneously (via `cover.control` with both fields), they are sent to the device in a single atomic command so the cover reaches the desired position and slat angle without a race between two sequential exchanges.
- Covers with a declared position-capable `io_device_type` automatically generate a companion Home Assistant button named `<Cover Name> Favorite Position`. That button sends the protocol's favorite or My-position command.
- Covers with `io_device_type` set to `window_opener` or `ventilation_point` also generate a companion button named `<Cover Name> Ventilation Position`. That button sends the protocol's ventilation command, which moves the actuator to a predefined partially-open position suitable for air exchange.
- Covers also automatically generate a diagnostic text sensor named `<Cover Name> Device Name`. That entity is disabled by default to avoid clutter. When enabled, it queues a boot-time `GET_NAME` protocol request, caches the returned UTF-8 device name, and publishes it to Home Assistant.
- Covers also automatically generate a diagnostic text sensor named `<Cover Name> Last Result`. Unlike the device-name sensor, this one is **enabled by default** — see "Last Command Result" below.
- Automatic favorite-button and ventilation-button generation is compile-time only. If `io_device_type` is omitted and learned later from radio traffic, the controller can still operate the cover normally, but it cannot add new ESPHome entities at runtime after boot.
- Automatic device-name and last-result sensor generation is also compile-time only for the same reason.
- The protocol support currently exposed here is one-way only: move to favorite. This component does not expose a sensor for reading the stored favorite position value, and it does not yet expose a save/delete favorite workflow because no verified controller-side protocol command has been identified.
- `status_poll_interval` is movement-scoped, not a continuous background refresh. The hub only keeps polling while a local command or overheard remote activity suggests that the device should still be changing, and it stops automatically once the device reports a stable state or the bounded polling window expires.
- After a STOP command the hub confirms the resting position via the same settle polling, capped to ~1 s (shorter than a normal move and any configured interval) so Home Assistant receives the final position quickly even when the device was still moving at the time of the stop.
- Many devices report a settle hint in their status replies; the actual next-poll delay is **min(device hint, configured interval)** — the device can shorten your configured interval but never stretch it. `status_poll_interval` is therefore a ceiling on the follow-up cadence. The hint value and chosen delay are visible at debug log level. When `status_poll_interval` is omitted the hint drives the interval directly (fallback 3 s when no hint is present). A STOP command always caps the settle to ~1 s regardless of the interval or hint.
- Unsolicited `0x71` device status updates are always applied to the entity state and extend settle polling while the device is still moving.
- Explicit `0xFE` device refusals are decoded into warn-level ESPHome logs and into the `Last Result` diagnostic sensor. Common examples include `LIMITATION_BY_RAIN`, `LIMITATION_BY_WIND`, and `THERMAL_PROTECTION`. See "Last Command Result" below.
- Devices that do not support `GET_NAME` simply keep an empty cached name. Name-request failures are intentionally isolated from normal control and status behavior.

## Home Assistant Actions

Home IO Control exposes hub-level actions through ESPHome's native API. These are intended for advanced workflows that should stay out of the default entity UI.

Required ESPHome API configuration:

```yaml
api:
  encryption:
    key: !secret api_key
```

Trigger an action from a Home Assistant automation or script by adding an `action:` step that calls the node-scoped ESPHome action name.

Generic pattern:

```yaml
action: esphome.<node_name>_<action_name>
data:
  ... action-specific fields ...
```

The component enables the required native API feature flags internally, so you do not need to add `custom_services:` or `homeassistant_services:` manually.

The first Home IO Control action is `rename_device`.

## Rename Device Action

When the node has `api:` enabled, the hub registers a native ESPHome action named `esphome.<node_name>_rename_device`.

`<node_name>` is derived from `esphome.name`, not `friendly_name`. Home Assistant normalizes that node name to snake case. For example, the sample V2 config uses `name: hioc-heltec-v2`, so the action becomes `esphome.hioc_heltec_v2_rename_device`.

Action fields:

- `device_id` (Required): Target IO-homecontrol device ID as exactly 6 hexadecimal characters. This is the protocol-level actuator ID, not the Home Assistant entity ID.
- `new_name` (Required): Requested device name as UTF-8 text. Leading and trailing ASCII whitespace is trimmed before validation. The final name must be representable in Latin-1 and fit within the protocol's 15-character write limit.

Behavior notes:

- The rename request uses the authenticated `SET_NAME` protocol exchange.
- The hub rejects unknown devices before transmitting anything.
- A successful protocol acknowledgement triggers an immediate `GET_NAME` readback through the same cached-name path used by the generated diagnostic text sensors.
- The result is considered `verified` only when that readback matches the normalized requested name exactly.
- If the target device returns `CMD_ERROR_RESP`, the hub surfaces the decoded result code in logs and in the emitted Home Assistant event.

Home Assistant event reporting:

- Every rename attempt fires `esphome.home_io_control_action_result`.
- Event fields always include `action`, `device_id`, `success`, `verified`, and `message`.
- Successful validations also include `requested_name` and, when available, `applied_name`.
- Explicit device refusals also include `result_code` and `result_code_name`.

Home Assistant Developer Tools -> Actions expects the direct action block below, without `alias:` or `sequence:`:

```yaml
action: esphome.hioc_heltec_v2_rename_device
data:
  device_id: "FEEB1E"
  new_name: "Patio Awning"
```

Use the same `device_id` value that you configured as `io_device_id` in the Home IO Control entity YAML. If you paired the device through discovery first, the pairing log also prints that same 6-character ID in the generated YAML snippet.

Example automation or script call:

```yaml
alias: Rename Patio Awning
sequence:
  - action: esphome.hioc_heltec_v2_rename_device
    data:
      device_id: "FEEB1E"
      new_name: "Patio Awning"
```

Home Assistant derives the action name from the node and the registered service name, so multiple Home IO Control hubs on different ESPHome nodes do not collide.

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

- `name` (Required): The entity name as shown in Home Assistant.
- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use the named value `light` when known, or a raw integer such as `0x06` if you are working from a pairing log that reports a not-yet-exposed alias. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the hub polls after commands or overheard remote activity at the device-reported settle hint (fallback 3 s) until the device reports a stable state or the bounded 10-minute window expires.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) that control this device. See the Linked Remotes section below for details.
- All standard options from the ESPHome light schema also apply.

Notes:

- This platform is intentionally binary only. Dimming is not exposed.
- The current implementation is still experimental and untested on local hardware in this repo.
- Known non-light device families will be rejected once the device type is known.
- Lights automatically generate a diagnostic text sensor named `<Light Name> Device Name`. That entity is disabled by default and uses the same cached-name behavior and boot-time `GET_NAME` request flow as the cover platform.
- Lights also automatically generate a `<Light Name> Last Result` diagnostic text sensor, enabled by default. See "Last Command Result" above.

## Lock Platform

Use the lock platform for IO-homecontrol lock devices that should appear in Home Assistant as native ESPHome locks.

```yaml
lock:
  - platform: home_io_control
    id: front_door_lock
    name: "Front Door Lock"
    io_device_id: "D0A9C0"
    io_device_type: "lock"
    status_poll_interval: 2s
```

Configuration variables:

- `name` (Required): The entity name as shown in Home Assistant.
- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use the named value `lock` when known, or a raw integer such as `0x09` if pairing reports a type without a named YAML alias yet. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the hub polls after commands or overheard remote activity at the device-reported settle hint (fallback 3 s) until the device reports a stable state or the bounded 10-minute window expires.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) that control this device. See the Linked Remotes section below for details.
- All standard options from the ESPHome lock schema also apply.

Notes:

- The current implementation exposes lock and unlock behavior and maps it onto the protocol's shared 0/100 execute encoding.
- The platform does not advertise ESPHome's optional `open` or code-required lock features because those command semantics are not validated here yet.
- The current implementation is experimental and untested on local hardware in this repo.
- Known non-lock device families will be rejected once the device type is known.
- Locks automatically generate a diagnostic text sensor named `<Lock Name> Device Name`. That entity is disabled by default and uses the same cached-name behavior and boot-time `GET_NAME` request flow as the cover platform.
- Locks also automatically generate a `<Lock Name> Last Result` diagnostic text sensor, enabled by default. See "Last Command Result" above.

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

- `name` (Required): The entity name as shown in Home Assistant.
- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use the named value `on_off_switch` when known, or a raw integer such as `0x0F` if pairing reports a type without a named YAML alias yet. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the hub polls after commands or overheard remote activity at the device-reported settle hint (fallback 3 s) until the device reports a stable state or the bounded 10-minute window expires.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) that control this device. See the Linked Remotes section below for details.
- All standard options from the ESPHome switch schema also apply.

Notes:

- This platform is also experimental and currently limited to binary on/off semantics.
- Known non-switch device families will be rejected once the device type is known.
- Switches automatically generate a diagnostic text sensor named `<Switch Name> Device Name`. That entity is disabled by default and uses the same cached-name behavior and boot-time `GET_NAME` request flow as the cover platform.
- Switches also automatically generate a `<Switch Name> Last Result` diagnostic text sensor, enabled by default. See "Last Command Result" above.

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
- This `button:` platform is only for the hub-level `Discover & Pair` action. Cover favorite buttons are generated automatically from eligible `cover:` entries and do not need a separate YAML block.

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

With `io_device_type: "awning"`, the example above also generates an `Awning Favorite Position` button automatically.

If `api:` is enabled, the same node also exposes `esphome.<node_name>_rename_device` for on-demand device renames.

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

With `io_device_type: "awning"`, the example above also generates an `Awning Favorite Position` button automatically.

If `api:` is enabled, the same node also exposes `esphome.<node_name>_rename_device` for on-demand device renames.

### Minimal Example with all Device Types: Cover, Light, Lock, and Switch

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

lock:
  - platform: home_io_control
    id: front_door_lock
    name: "Front Door Lock"
    io_device_id: "D0A9C0"
    io_device_type: "lock"
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
- [config/heltec-wifi-lora-32-v2-all-types.yaml](../config/heltec-wifi-lora-32-v2-all-types.yaml): SX1276 Heltec LoRa32 V2 controller config without OLED support that exercises every currently supported ESPHome platform in this component: cover, light, lock, switch, and the Discover & Pair button, all with dummy device IDs ready to replace.
- [config/heltec-wifi-lora-32-v3.yaml](../config/heltec-wifi-lora-32-v3.yaml): SX1262 Heltec WiFi LoRa32 V3/V3.2 controller config with one awning cover, a Discover & Pair button, and an OLED status display tuned for the V3 pinout and TCXO settings.
- [config/heltec-wifi-lora-32-v3-monitor.yaml](../config/heltec-wifi-lora-32-v3-monitor.yaml): SX1262 passive monitor config for Heltec WiFi LoRa32 V3/V3.2 that keeps the radio in RX, enables `IOHOME_FRAME_LOG`, and logs parsed traffic without creating entities or exposing a pairing button.

## Diagnostics and Unknown Position

- Warn-level logs now decode explicit `CMD_ERROR_RESP (0xFE)` replies instead of collapsing them into generic command failures. A refused command will look like `Device ABC123: command 0x00 returned limitation result=0xEB LIMITATION_BY_WIND (parameter was limited by a wind sensor)`.
- The component's internal protocol layer uses `212.0F` as an unknown-position sentinel (matching `POS_UNKNOWN (0xD4)`), but this value is **never** exposed through the ESPHome cover's `position` field. Cover entities start at `1.0` (fully open) and update to the real device position once the first status response arrives. Custom lambdas can therefore treat any value in [0.0, 1.0] as valid.
- The OLED example configs in this repo render `--` plus `Unknown` when a cover position is out of range, which now only applies before the first status poll completes on a fresh boot.

### Last Command Result

Every device-bound platform (cover, light, switch, lock) automatically generates a companion diagnostic text sensor named `<Entity Name> Last Result`. Unlike the `Device Name` sensor, it is **enabled by default**, since it turns a silent "nothing happened" command into a self-explained one — for example, pressing "open" on an awning during high wind now shows `LIMITATION_BY_WIND` in Home Assistant instead of only a log line.

- The sensor publishes the symbolic result name (`command_result_name()`), such as `LIMITATION_BY_RAIN`, `LIMITATION_BY_WIND`, or `THERMAL_PROTECTION` — the same names that appear in the warn-level log line above.
- It publishes an empty string when no `CMD_ERROR_RESP` has been recorded yet, and again once the device replies successfully to a later status poll or command — a stale limitation reason from an hour ago is worse than none, so it does not linger once the device is confirmed working again.
- Both the unsolicited path (a device reporting a limitation on its own, e.g. after a wind gust) and the direct reply to a Home Assistant command are recorded, so this sensor also covers cases where the command was rejected outright.

Example custom-lambda pattern:

```yaml
lambda: |-
  const float position = id(patio_awning).position;
  if (position < 0.0f || position > 1.0f) {
    ESP_LOGI("example", "position is unknown");
  } else {
    ESP_LOGI("example", "position %.0f%%", position * 100.0f);
  }
```

## Pairing Workflow

1. Choose a controller `node_id` (any unique 6-character hex string) and `system_key` (32-character hex string). Keep these consistent across firmware updates.
2. Flash the firmware with at least the `home_io_control:` hub and a `button:` entity configured.
3. Put exactly **one** target device into pairing mode by pressing its PROG button. The pairing window is short — typically a few seconds.
4. Press the **Discover & Pair** button entity in Home Assistant within that window.
5. Watch the ESPHome logs. On success you will either get a ready-to-paste YAML snippet with `io_device_id`, `io_device_type`, and `io_subtype`, or a follow-up message explaining why a snippet could not be generated.
6. Copy `io_device_type` and `io_subtype` from the log snippet and add them, together with `io_device_id`, to the appropriate `cover:`, `light:`, `lock:`, or `switch:` entry in your YAML. If the log uses a raw numeric type such as `0x11`, keep that exact value in YAML.
7. Reflash with the updated YAML. The entity will appear in Home Assistant and the controller will begin polling the device for status.
8. If pairing reports that the type is unsupported or that discovery metadata was incomplete, follow the log guidance and please file a GitHub issue with the raw type/subtype, device model, and pairing log so support can be added.

**Tips:**

- Press the device's PROG button first, then press "Discover & Pair" in Home Assistant within 2–3 seconds. The device's pairing window is time-limited.
- The controller retries discovery up to 3 times per button press. If pairing fails on the first press, try again — the timing between PROG and Discover & Pair matters.
- If a device in pairing mode does not respond to the default discovery command, use the `tuning:` block to experiment with alternate discovery commands and radio timings. See [Radio Diagnostics Tuning](radio_diagnostics.md).

### Diagnosing a failed pairing attempt

Every `home_io_control` config with a `button:` entity automatically gets a companion
**"Last Pairing Result"** diagnostic text sensor — no YAML configuration needed. It updates
after every "Discover & Pair" attempt with a frozen, machine-readable summary:

```
v1; outcome=paired; phase=complete; node=30E1F2; type=awning; attempts=1; lbt=0; dur_ms=842; heard=3; advice=none
```

| Field | Meaning |
|-------|---------|
| `outcome` | `paired`, `no_response`, `invalid_response`, `key_exchange_failed`, or `config_failed` (key exchange succeeded but the best-effort SetConfig1 step failed — still counted as paired). |
| `phase` | The pairing state machine's furthest-reached stage. |
| `node` / `type` | The paired device's node ID and type, or `-` if nothing was paired. |
| `attempts` | Number of discovery command retries sent. |
| `lbt` | Listen-before-talk retries consumed across the whole attempt (channel-busy indicator). |
| `dur_ms` | Attempt duration in milliseconds. |
| `heard` | Total RX events seen, including ones rejected by the pairing classifiers. |
| `advice` | Comma-separated advisor codes (see below), or `none`. |

This is intentionally a stable, parseable format (the `v1;` prefix is versioned) so it can be
scripted against — e.g. an automation that alerts if `outcome` isn't `paired`.

At the end of every attempt, the ESPHome log also gets a full human-readable summary (every
TX/RX/RX_REJECT/LBT-defer/hop/phase event, in order) plus, when applicable, one or more
**pairing advisor** WARN lines that turn overheard radio traffic into an actionable diagnosis:

| Advice code | When it fires | What it means |
|-------------|----------------|----------------|
| `1w_traffic` | A 1W remote is seen performing 1W pairing (CTRL0 1W bit set, broadcast to `00003F`, with a 1W pairing command byte) to the same device you're trying to pair. | The motor is **not** in 2W learning mode — a PROG press on a 1W remote does not enable 2W discovery. This is the issue #27 case: perform a Double Power Cut on the motor to force 2W learning mode, then retry. |
| `channel_busy` | Listen-before-talk retries were exhausted (reached the configured max) and the same source was heard repeatedly during the wait. | The channel is being flooded by a repeating beacon (usually a nearby remote or sensor); discovery transmissions were delayed. Try again, or tune `lbt_max_retries`/`lbt_rssi_threshold_dbm` — see [Radio Diagnostics Tuning](radio_diagnostics.md). |
| `foreign_controller` | A discovery response (0x29) was seen addressed to a node ID that isn't this controller's. | Another controller (e.g. TaHoma) is pairing the same device right now. Wait for it to finish, or make sure you're the only controller with the device in pairing mode. |
| `rf_silent` | Nothing at all was heard on any channel during the whole discovery window. | Distinguishes "RF dead" (antenna, wiring, wrong channel/tuning) from "device just isn't in pairing mode" — check the antenna and radio tuning before re-pressing PROG. |

## Device Type and Capability Notes

- **Cover-like families** (shutters, awnings, blinds, openers, curtains) are the primary supported path today. These support full position control (0–100%).
- **Binary light, lock, and switch** support exists, but remains experimental and has not been validated against real hardware.
- **Raw type IDs in YAML**: `io_device_type` accepts both named values such as `awning` and raw integers such as `0x11`. Raw values are useful when pairing discovers a valid IO-homecontrol type that this project does not yet expose under a named YAML alias.

### Named device types

Both `io_device_type` and the `class:<device_type>` form of `linked_remotes` (see Linked Remotes below) accept these named values:

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

A device type not in this table can still be declared as a raw hex ID, in either place: `io_device_type: 0x14` or `linked_remotes: ["class:0x14"]`. Named and raw-hex entries can be mixed freely within the same `linked_remotes` list.

**Finding your device's type:** the surest way is to pair it through Home Assistant and check the "Last Pairing Result" diagnostic sensor — its `type=` field reports the device's actual type name (see "Pairing Workflow" above, e.g. `type=awning`). If you already know the device (e.g. you're configuring a Somfy awning), just use the matching name from the table above.

## Linked Remotes

Physical IO-Homecontrol remotes (wall switches, handheld remotes, wind sensors) use the 1W (one-way) protocol to send commands. Unlike 2W devices that address a specific device ID, 1W remotes broadcast to a type-class address (e.g., "all awning devices"). This means the controller cannot automatically detect which of your devices a particular remote controls — you need to configure the link explicitly.

**This is receive-only.** The hub reacts to physical 1W remotes and sensors it overhears — decoding intent, firing events, updating linked devices — but it does not transmit 1W commands itself, and it does not control 1W-only devices (devices that have no 2W/authenticated protocol at all). Everything on this page assumes the devices you control are 2W, and that `linked_remotes` / `exposed_senders` only change how the hub *reacts* to radio traffic it overhears from other transmitters.

### Why link a remote?

Without `linked_remotes`, when someone presses a wall remote, the device moves but the controller doesn't know about it until the next scheduled poll. With `linked_remotes` configured, the controller overhears the remote's radio traffic and immediately polls the device for its new position (with a configurable poll interval).

### Finding your remote's node ID

1. Set the logger to DEBUG level in your YAML:

```yaml
logger:
  level: DEBUG
```

2. Flash the firmware and open the serial log or ESPHome logs.

3. Press a button on the physical remote.

4. Look for a log line like:

```
[D][home_io_control] rx 1W remote 9D6085 targets all: EXECUTE(0x00) CLOSE originator=user_remote priority=user_high
```

5. The 6-character hex ID after `1W remote` is your remote's node ID — in this example, `9D6085`.

**Notes:**
- Each button press may produce multiple log lines (a CMD 0x20 beacon and a CMD 0x00 execute). The source ID is the same on all of them — use either one.
- Wind and rain sensors also show up with `originator=wind_sensor` or `originator=rain_sensor`. These can be linked the same way if you want the controller to react to sensor-triggered movements.
- If you have multiple remotes, press each one separately and note which ID appears for each.
- Devices nearby on the same frequency will also appear in the log. If you're unsure which ID belongs to your remote, press the button a few times and look for the ID that consistently appears at the same time.

### Configuring linked remotes

Add the remote's node ID to the `linked_remotes` list on the entity it controls:

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    io_device_id: "30E1F2"
    io_device_type: "awning"
    linked_remotes:
      - "9D6085"
```

Multiple remotes can be linked to the same device, and the same remote can be linked to multiple devices if it controls more than one:

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    io_device_id: "30E1F2"
    linked_remotes:
      - "9D6085"
      - "A0A9A1"

  - platform: home_io_control
    name: "Bedroom Shutter"
    io_device_id: "054E17"
    linked_remotes:
      - "9D6085"
```

### Linking by device class

Instead of (or alongside) individual remote node IDs, a `linked_remotes` entry can be `class:<device_type>`, matching how 1W remotes actually address a device — a typed broadcast such as "all awnings" rather than a single node. This links *every* typed broadcast targeting that class to the device, without enumerating each remote's node ID:

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    io_device_id: "30E1F2"
    io_device_type: "awning"
    linked_remotes:
      - "class:awning"
```

`<device_type>` accepts the same named values as `io_device_type` — see the "Named device types" table under Device Type and Capability Notes above for the full list (e.g. `awning`, `roller_shutter`, `venetian_blind`) — or a raw hex ID such as `class:0x14` for a type without a named alias yet. Bare node-ID entries, named `class:` entries, and raw-hex `class:` entries can all be mixed freely in the same list, and a device linked both ways (by ID and by class) is only updated once per press — the class form is purely a convenience for "any remote that presses this device's type," it does not change how bare node IDs behave.

### Optimistic state

By default (`optimistic_state: true`), a linked remote's press — or an HA-issued open/close/stop/set-position command — shows the requested direction in Home Assistant immediately, before the confirming poll (linked remote) or device response (HA command) arrives. This is a pure UX bridge: the confirming poll/response always still runs and overwrites the optimistic value with the device's real reported position. Set `optimistic_state: false` on a device to disable this and fall back to polling only.

Notes:
- **Tilt-only commands are not optimistic.** Setting only a tilt value (no position) queues the command normally but does not show an optimistic state change, since tilt has no equivalent "direction" concept in the cover UI. A combined position+tilt command *does* apply the optimistic position target.
- **Class-linked devices are safety-filtered.** When a typed broadcast (e.g. "all awnings") also fans out to `class:`-linked devices, a device is only optimistically moved if its own declared `io_device_type` matches the broadcast's target type — it is never skipped for polling, only for the optimistic nudge. This prevents an "all awnings" press from optimistically moving a device you've linked to that class but declared as a different type.

### What the decoded log tells you

| Field | Meaning |
|-------|---------|
| `1W remote XXXXXX` | The remote's 6-character node ID |
| `targets all` / `targets awning` | The broadcast device-type class the remote addresses |
| `EXECUTE(0x00) CLOSE` | The command: OPEN, CLOSE, STOP, FAVORITE, VENT, or a numeric position |
| `originator=user_remote` | Who triggered the command (user, wind sensor, rain sensor, timer, etc.) |
| `priority=user_high` | The ACEI priority level of the command |
| `(linked → 30E1F2)` | Shown when the remote is already configured — confirms the link is active |
- **Device type learning**: The YAML-declared `io_device_type` is the permanent, authoritative type. The controller may still learn a device's type from radio for runtime profile selection when the type is not declared in YAML, but it will never overwrite a YAML-declared type.
- **Inversion defaults**: Some device families (e.g., horizontal awnings) default to inverted position mapping. When `invert_position` is omitted, the cover entity follows that learned device profile automatically. Setting `invert_position` explicitly overrides the learned value.
- Additional reference-derived device types such as heating devices, sensors, and beacons are recognized for classification and logging, but they do not yet have dedicated ESPHome platform support.

## Sender Events (remote buttons & sensors)

Every decoded 1W transmission is DEBUG-logged (see the Linked Remotes section above), regardless of configuration. Additionally, senders on the `exposed_senders` allowlist fire an `esphome.home_io_control_sender_event` event to Home Assistant, so you can trigger automations directly from a physical remote press — including a remote that doesn't control any device you own an entity for (e.g. a spare remote you want to repurpose as an HA trigger).

"Sender" is deliberately not "remote": handheld/wall remotes and wind/rain sensors use the exact same 1W broadcast mechanism and the same node-ID addressing — a wind sensor is just another device that broadcasts a command (e.g. "close due to wind") with `originator=wind_sensor` instead of `originator=user_remote`. From the radio's perspective they're indistinguishable except for that one payload byte, so `exposed_senders` (like `linked_remotes`) works identically for either: a wind or rain sensor's node ID can go in this list the same way a handheld remote's can.

### Why this is opt-in

1W frames are unencrypted broadcasts to a type-class address (e.g., "all awning devices") — there is no ownership marker on the radio protocol. Your controller can overhear a neighbor's remote (or sensor) as easily as your own if it's in range. Firing an event to Home Assistant for every overheard transmission could mean HA sees a neighbor's button presses, so `exposed_senders` defaults to an **empty list**: nothing fires an event until you explicitly add its node ID.

This is a separate mechanism from `linked_remotes` (which drives optimistic status polling for a device you own):

- A sender can be in `exposed_senders` without being linked to any device — useful for a remote or sensor you want purely as an HA trigger.
- A sender can be linked to a device without being in `exposed_senders` — linking still triggers a status poll, but the transmission does **not** reach Home Assistant as an event.
- A sender can be in both lists at once.

### Configuring exposed senders

Find the sender's node ID the same way as for `linked_remotes` (see "Finding your remote's node ID" above — the same log line works for a wind/rain sensor's node ID too), then add it to the hub-level `exposed_senders` list:

```yaml
home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"
  exposed_senders:
    - "9D6085"
```

### Event data

| Field | Meaning |
|-------|---------|
| `remote_id` | The sender's 6-character node ID |
| `target_class` | Address classification: `unicast`, `broadcast_all`, `broadcast_type`, `discovery`, or `unknown_broadcast` |
| `target_type` | The broadcast device-type class the sender addresses (e.g. `awning`, or `unknown` for an all-devices broadcast) |
| `cmd` | The command name and hex code, e.g. `execute(0x00)` |
| `intent` | The decoded command: `OPEN`, `CLOSE`, `STOP`, `FAVORITE`, `VENT`, or a numeric position |
| `originator` | Who triggered the command (`user_remote`, `wind_sensor`, `rain_sensor`, `timer`, etc.) |
| `acei_level` | The ACEI priority level of the command |
| `linked` | `"true"` if this sender is also in some entity's `linked_remotes` list, `"false"` otherwise |

### Verifying the event fires

Before wiring up an automation, confirm the event actually reaches Home Assistant:

1. In Home Assistant, go to **Developer Tools → Events**.
2. Under "Listen to events", type `esphome.home_io_control_sender_event` and click **Start listening**.
3. Press the physical remote button (or trigger the sensor). If everything is configured correctly, the event and its full data payload appear in that panel within a second or two.
4. Leave that panel open while iterating on `exposed_senders` — no reflash is needed to test, just re-press the remote after saving a config change and reflashing.

If nothing appears, check the ESPHome DEBUG log (`logger: level: DEBUG`) for one of these lines, logged right after the `rx 1W remote ...` decode line:

| Log line | Meaning |
|----------|---------|
| `Firing esphome.home_io_control_sender_event for sender XXXXXX` | The event was sent — if Developer Tools still shows nothing, check the Home Assistant API connection instead (`api:` block, encryption key, network). |
| `1W sender XXXXXX has intent but is not in exposed_senders, skipping ...` | The sender ID isn't on the allowlist (or doesn't match — check exact casing/value). |
| `1W sender XXXXXX has intent but the API is not connected, skipping ...` | Home Assistant hasn't got an active connection to this device yet. |
| *(no line at all, just the `rx 1W remote ...` decode)* | This particular frame carried no decodable command intent — see below. |

**Not every button press fires the event.** Only frames decoded as `CMD_EXECUTE` or `CMD_ACTIVATE_MODE` carry a command intent (`OPEN`/`CLOSE`/`STOP`/etc.); other 1W traffic from the same remote — for example a `WRITE_PRIVATE(0x20)` frame, which some remotes send as part of the same button press — is still DEBUG-logged but never fires the event, because there is nothing decodable to put in `intent`. If you only ever see `WRITE_PRIVATE` lines and never an `EXECUTE` line for a press that should have moved something, the `EXECUTE` frame itself was likely never received (radio timing/contention), not silently dropped by this component.

### Example automation

```yaml
automation:
  - alias: "Awning remote pressed"
    trigger:
      - platform: event
        event_type: esphome.home_io_control_sender_event
        event_data:
          remote_id: "9D6085"
    action:
      - service: notify.mobile_app
        data:
          message: "Awning remote: {{ trigger.event.data.intent }}"
```

## See Also

- [ESPHome External Components](https://esphome.io/components/external_components/)
- [ESPHome Cover Component](https://esphome.io/components/cover/)
- [ESPHome Light Component](https://esphome.io/components/light/)
- [ESPHome Lock Component](https://esphome.io/components/lock/)
- [ESPHome Switch Component](https://esphome.io/components/switch/)
- [ESPHome Button Component](https://esphome.io/components/button/)