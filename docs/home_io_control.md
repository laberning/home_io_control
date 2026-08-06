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
  clk_pin:
    number: 5
    ignore_strapping_warning: true
  mosi_pin: 27
  miso_pin: 19

home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  radio_type: sx1276
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

The `home_io_control:` block defines the shared radio/controller hub. All cover, light, lock, switch, and button entities attach to this hub.

```yaml
home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  radio_type: sx1276
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"
```

Configuration variables:

- `id` (Optional): Manually specify the hub ID for code generation. Use this when entity blocks should reference a specific hub with `home_io_control_id`.
- `cs_pin` (Required): SPI chip select pin for the radio.
- `rst_pin` (Required): Radio reset pin.
- `dio0_pin` (Optional): SX1276 DIO0 interrupt pin.
- `dio4_pin` (Optional): SX1276 DIO4 preamble-detect pin. Most boards do not wire this.
- `dio1_pin` (Optional): The chip's IRQ line — SX1262 DIO1, or LR1121 DIO9.
- `busy_pin` (Optional): SX1262/LR1121 BUSY pin.
- `node_id` (Required): 3-byte controller ID as exactly 6 hexadecimal characters.
- `system_key` (Required): 16-byte installation key as exactly 32 hexadecimal characters.
- `tx_power` (Optional, default: `17`): Radio transmit power. The schema currently accepts `0` to `22`.
- `pa_pin` (Optional, default: `BOOST`): SX1276 PA path. Valid values are `BOOST` and `RFO`.
- `radio_type` (Required): The radio chip fitted to your board. Valid values are `sx1276`, `sx1262`, and `lr1121` — see "Radio Chip Support" below.
- `fem_en_pin` (Optional): Front-end module enable pin for boards with an external RF front-end.
- `vfem_pin` (Optional): Front-end module power pin for boards with an external RF front-end.
- `fem_pa_pin` (Optional): Front-end module PA select pin for boards with an external RF front-end.
- `tcxo_voltage` (Optional, default: `1_8V`): SX1262/LR1121 TCXO voltage. Valid values are `1_6V`, `1_7V`, `1_8V`, `2_2V`, `2_4V`, `2_7V`, `3_0V`, and `3_3V`.
- `exposed_senders` (Optional, default: empty list): List of 1W sender node IDs (6 hex characters each — remotes *or* sensors, see below) allowed to fire the `esphome.home_io_control_sender_event` event to Home Assistant. Empty by default — see the Sender Events section below for why this is opt-in and how it relates to `linked_remotes`.
- `tuning` (Optional): Diagnostics block for pairing/radio parameters. See [Radio Diagnostics Tuning](radio_diagnostics.md).

Notes:

- The SPI bus itself is configured separately in the top-level `spi:` block.
- The component extends ESPHome's SPI device schema, so standard SPI-device options apply in addition to the keys above.
- Some boards route the SPI bus through an ESP32 strapping pin (GPIO5's `clk_pin` on classic ESP32, GPIO3's `miso_pin` on ESP32-S3). ESPHome refuses to reuse a strapping pin as a plain GPIO unless you set `ignore_strapping_warning: true` on that pin's expanded schema, e.g. `clk_pin: {number: 5, ignore_strapping_warning: true}` — see the examples below.
- SX1276 uses a different interrupt pin set (`dio0_pin`/`dio4_pin`) than SX1262/LR1121 (`dio1_pin`/`busy_pin`, shared — LR1121 reuses the same two keys, with `dio1_pin` carrying its DIO9 line). Only configure the pins for the radio you actually have.
- User control commands (position, STOP, tilt, light/switch/lock state) are prioritized over background status polls in the operation queue. A queued status poll for the same device is dropped when a control command arrives, since the command reply provides fresher state. An in-flight exchange cannot be interrupted, so the worst-case latency is one full exchange (~1–3 s) regardless of the queue.

### Radio-Specific Pin Requirements

| Radio | Required hub pins | Optional hub pins | Typical extra setting |
| --- | --- | --- | --- |
| SX1276 | `cs_pin`, `rst_pin`, `dio0_pin` | `dio4_pin` | `radio_type: sx1276`, `pa_pin: BOOST` |
| SX1262 | `cs_pin`, `rst_pin`, `dio1_pin`, `busy_pin` | `fem_en_pin`, `vfem_pin`, `fem_pa_pin` | `radio_type: sx1262`, `tcxo_voltage: 1_8V` |
| LR1121 | `cs_pin`, `rst_pin`, `dio1_pin`, `busy_pin` | — | `radio_type: lr1121`, `tcxo_voltage: 3_0V` |

### Radio Chip Support

SX1276, SX1262, and LR1121 are all confirmed and validated on real hardware (see the [README hardware table](../README.md#hardware-requirements)). `radio_type` must always be set explicitly — there is no chip auto-detection, so an ambiguous or mis-wired chip can never be silently probed into the wrong SPI command set.

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
- Covers also automatically generate a diagnostic text sensor named `<Cover Name> Active Issue`. Unlike the device-name sensor, this one is **enabled by default** — see "Active Issue" below.
- Covers also automatically generate three disabled-by-default `<Cover Name> RSSI` / `Last Contact` / `Exchange Failures` diagnostic sensors. See "Link Health" below.
- Automatic favorite-button and ventilation-button generation is compile-time only. If `io_device_type` is omitted and learned later from radio traffic, the controller can still operate the cover normally, but it cannot add new ESPHome entities at runtime after boot.
- Automatic device-name, active-issue, and link-health sensor generation is also compile-time only for the same reason.
- The protocol support currently exposed here is one-way only: move to favorite. This component does not expose a sensor for reading the stored favorite position value, and it does not yet expose a save/delete favorite workflow because no verified controller-side protocol command has been identified.
- `status_poll_interval` is movement-scoped, not a continuous background refresh. The hub only keeps polling while a local command or overheard remote activity suggests that the device should still be changing, and it stops automatically once the device reports a stable state or the bounded polling window expires.
- After a STOP command the hub confirms the resting position via the same settle polling, capped to ~1 s (shorter than a normal move and any configured interval) so Home Assistant receives the final position quickly even when the device was still moving at the time of the stop.
- Many devices report a settle hint in their status replies; the actual next-poll delay is **min(device hint, configured interval)** — the device can shorten your configured interval but never stretch it. `status_poll_interval` is therefore a ceiling on the follow-up cadence. The hint value and chosen delay are visible at debug log level. When `status_poll_interval` is omitted the hint drives the interval directly (fallback 3 s when no hint is present). A STOP command always caps the settle to ~1 s regardless of the interval or hint.
- Unsolicited `0x71` device status updates are always applied to the entity state and extend settle polling while the device is still moving.
- Explicit `0xFE` device refusals are decoded into warn-level ESPHome logs and into the `Active Issue` diagnostic sensor. Common examples include `LIMITATION_BY_RAIN`, `LIMITATION_BY_WIND`, and `THERMAL_PROTECTION`. See "Active Issue" below.
- Devices that do not support `GET_NAME` simply keep an empty cached name. Name-request failures are intentionally isolated from normal control and status behavior.

## Home Assistant Actions

Beyond the entities generated from your `cover:`/`light:`/`lock:`/`switch:` YAML, Home IO Control exposes three **hub-level actions** through ESPHome's native API. These are one-off/advanced operations that would clutter the entity UI if they were always-visible buttons, so they're only reachable via Developer Tools or an automation.

| Action | What it does | `verified` can be `true`? |
|---|---|---|
| `rename_device` | Renames a paired actuator and reads the name back to confirm the write. | Yes |
| `identify_device` | Makes a device physically identify itself (brief jog/flash) so you can tell which physical motor a device ID belongs to. | No — no readback exists for a jog |
| `force_open_device` ⚠️ *experimental* | Requests a fully-open move at elevated protocol priority, intended to override wind/rain soft locks. Confirmed to move the device correctly; **not yet confirmed to actually override an active lock** — see the warning below. | No — the outcome is asynchronous |

### Enabling and triggering actions

Requires a normal `api:` block — Home IO Control enables the extra native-API feature flags this needs internally, so no `custom_services:` or `homeassistant_services:` is needed:

```yaml
api:
  encryption:
    key: !secret api_key
```

Each action becomes a node-scoped ESPHome action named `esphome.<node_name>_<action_name>`. `<node_name>` comes from `esphome.name` (not `friendly_name`), which Home Assistant normalizes to snake_case — e.g. the sample V2 config uses `name: hioc-heltec-v2`, so `rename_device` becomes `esphome.hioc_heltec_v2_rename_device`.

**From Home Assistant Developer Tools -> Actions**, use the direct action block (no `alias:`/`sequence:`):

```yaml
action: esphome.hioc_heltec_v2_identify_device
data:
  device_id: "FEEB1E"
```

**From an automation or script**, wrap the same block in a normal step:

```yaml
alias: Identify the Patio Awning
sequence:
  - action: esphome.hioc_heltec_v2_identify_device
    data:
      device_id: "FEEB1E"
```

Every action takes `device_id`: the 6-hex-character IO-homecontrol device ID — the same value you set as `io_device_id` in the entity's YAML, and the same ID the pairing log prints. This is the protocol-level actuator ID, **not** the Home Assistant entity ID.

### Result events

Every action fires the same Home Assistant event, `esphome.home_io_control_action_result`, so one automation trigger can react to any of them:

| Field | When present | Meaning |
|---|---|---|
| `action` | always | Action name, e.g. `rename_device`. |
| `device_id` | always | Target device ID. |
| `success` | always | Whether the action succeeded. |
| `verified` | always | Whether a follow-up readback confirmed the result — see the table above for which actions can ever set this `true`. |
| `message` | always | Human-readable outcome summary. |
| `requested_name`, `applied_name` | `rename_device` only | Requested vs. verified device name. |
| `result_code`, `result_code_name` | `rename_device` and `identify_device` only, when the device replies `CMD_ERROR_RESP` | Decoded protocol result code. |

### `rename_device`

Fields: `device_id` (required), `new_name` (required — UTF-8, ASCII whitespace trimmed, must fit the protocol's 15-character Latin-1 write limit).

Sends the authenticated `SET_NAME` write, then immediately sends `GET_NAME` to read it back — `verified` is `true` only if the readback matches the requested name exactly. An explicit device refusal (`CMD_ERROR_RESP`) surfaces its decoded result code in both the logs and the event.

```yaml
action: esphome.hioc_heltec_v2_rename_device
data:
  device_id: "FEEB1E"
  new_name: "Patio Awning"
```

### `identify_device`

Fields: `device_id` (required).

Sends the authenticated `CMD_IDENTIFY` command. No device-type gating beyond "is it registered" — identify exists specifically to help you work out what an unknown or unrecognized device physically is. A `CMD_ERROR_RESP` reply still counts as **success**: some devices answer that way to an identify request and jog anyway (confirmed on real hardware — the awning jogged every time despite the error reply).

```yaml
action: esphome.hioc_heltec_v2_identify_device
data:
  device_id: "FEEB1E"
```

### `force_open_device`

Fields: `device_id` (required).

Queued through the same dispatch path as the cover entity and its buttons (capability gating, poll tracking, settle handling, backoff) rather than sent directly, so a non-cover device is rejected the same way any other cover command would be. The result event only confirms the command was **queued** — the actual movement shows up later through the device's normal cover-state/polling pipeline, the same as any other cover command.

```yaml
action: esphome.hioc_heltec_v2_force_open_device
data:
  device_id: "FEEB1E"
```

## Light Platform

Use the light platform for IO-homecontrol light devices. Defaults to binary on/off; set `dimmable: true` for brightness control on devices that support intermediate positions.

```yaml
light:
  - platform: home_io_control
    id: garden_light
    name: "Garden Light"
    io_device_id: "D15C05"

  - platform: home_io_control
    id: dimmable_light
    name: "Dimmable Light"
    io_device_id: "D15C06"
    dimmable: true
```

Configuration variables:

- `name` (Required): The entity name as shown in Home Assistant.
- `home_io_control_id` (Optional): Reference to the `home_io_control` hub to use.
- `io_device_id` (Required): 3-byte IO-homecontrol device ID as exactly 6 hexadecimal characters.
- `io_device_type` (Optional): Declare the IO-homecontrol device type. Use the named value `light` when known, or a raw integer such as `0x06` if you are working from a pairing log that reports a not-yet-exposed alias. When omitted, the controller may learn the type later from radio metadata.
- `io_subtype` (Optional): Device subtype value (0–63), as reported by the device. When omitted, the controller may learn it later from radio metadata.
- `dimmable` (Optional, default `false`): Expose brightness control (`ColorMode::BRIGHTNESS`) instead of on/off only. The protocol has no machine-readable signal for whether a given light device actually supports intermediate positions, so this is an explicit opt-in — leave it unset for binary-only devices. Brightness is applied instantly (no client-side fade) by default, since ESPHome's default 1s transition would otherwise send a stream of superseding radio commands over the device's round trip; override `default_transition_length` in YAML if you want a fade anyway.
- `status_poll_interval` (Optional): Poll interval used for bounded follow-up status checks while this device is expected to be changing state. The minimum supported value is 500ms. When omitted, the hub polls after commands or overheard remote activity at the device-reported settle hint (fallback 3 s) until the device reports a stable state or the bounded 10-minute window expires.
- `linked_remotes` (Optional): List of remote node IDs (6 hex characters each) that control this device. See the Linked Remotes section below for details.
- All standard options from the ESPHome light schema also apply.

Notes:

- Binary on/off has been validated against real hardware (a Somfy Izymo dimmer, used in binary mode). `dimmable: true` has also been validated on the same device for intermediate brightness levels.
- Known non-light device families will be rejected once the device type is known.
- Lights automatically generate a diagnostic text sensor named `<Light Name> Device Name`. That entity is disabled by default and uses the same cached-name behavior and boot-time `GET_NAME` request flow as the cover platform.
- Lights also automatically generate a `<Light Name> Active Issue` diagnostic text sensor, enabled by default. See "Active Issue" below.
- Lights also automatically generate three disabled-by-default `<Light Name> RSSI` / `Last Contact` / `Exchange Failures` diagnostic sensors. See "Link Health" below.

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
- Locks also automatically generate a `<Lock Name> Active Issue` diagnostic text sensor, enabled by default. See "Active Issue" below.
- Locks also automatically generate three disabled-by-default `<Lock Name> RSSI` / `Last Contact` / `Exchange Failures` diagnostic sensors. See "Link Health" below.

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
- Switches also automatically generate a `<Switch Name> Active Issue` diagnostic text sensor, enabled by default. See "Active Issue" below.
- Switches also automatically generate three disabled-by-default `<Switch Name> RSSI` / `Last Contact` / `Exchange Failures` diagnostic sensors. See "Link Health" below.

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
  clk_pin:
    number: 5
    ignore_strapping_warning: true
  mosi_pin: 27
  miso_pin: 19

external_components:
  - source: github://laberning/home_io_control

home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  radio_type: sx1276
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

### Minimal LR1121 Cover Controller

```yaml
esphome:
  name: io-homecontrol-lr1121

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
  clk_pin: 5
  mosi_pin: 6
  miso_pin:
    number: 3
    ignore_strapping_warning: true

external_components:
  - source: github://laberning/home_io_control

home_io_control:
  cs_pin: 7
  rst_pin: 8
  dio1_pin: 36  # LR1121 DIO9
  busy_pin: 34
  radio_type: lr1121
  tcxo_voltage: 3_0V
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

### Minimal Example with all Device Types: Cover, Light, Lock, and Switch

```yaml
home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  radio_type: sx1276
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

### Active Issue

Every device-bound platform (cover, light, switch, lock) automatically generates a companion diagnostic text sensor named `<Entity Name> Active Issue`. Unlike the `Device Name` sensor, it is **enabled by default**, since it turns a silent "nothing happened" command into a self-explained one — for example, pressing "open" on an awning during high wind now shows `LIMITATION_BY_WIND` in Home Assistant instead of only a log line.

This is not a per-operation result — it does not get set on every command, only while an actual issue is outstanding.

- The sensor publishes the symbolic result name (`command_result_name()`), such as `LIMITATION_BY_RAIN`, `LIMITATION_BY_WIND`, or `THERMAL_PROTECTION` — the same names that appear in the warn-level log line above.
- It publishes an empty string when no `CMD_ERROR_RESP` has been recorded yet, and again once the device replies successfully to a later status poll or command — a stale limitation reason from an hour ago is worse than none, so it does not linger once the device is confirmed working again.
- Both the unsolicited path (a device reporting a limitation on its own, e.g. after a wind gust) and the direct reply to a Home Assistant command are recorded, so this sensor also covers cases where the command was rejected outright.

### Link Health

Every device-bound platform also automatically generates three per-device diagnostic `sensor:` entities for radio and exchange health. Unlike the `Active Issue` sensor, all three are **disabled by default** — they are lower-level radio diagnostics, not everyday values, so they stay out of the way until explicitly enabled in Home Assistant.

- **`<Entity Name> RSSI`** (dBm): a smoothed (exponential moving average, 1/8 weight per sample) signal-strength reading, updated on every frame received from the device — replies to hub commands and unsolicited device-initiated traffic alike. Shows as unavailable until the first frame is received; a real 0 dBm reading is never fabricated as a placeholder.
- **`<Entity Name> Last Contact`** (seconds): seconds elapsed since the last frame received from the device — an age, not a timestamp, and not limited to traffic the device sends on its own: replies to the hub's own status polls and commands count too. It resets to ~0 on every such frame and counts up from there while the device is quiet, republished once a minute by its own heartbeat so it keeps advancing in Home Assistant even between frames. The hub has no wall-clock time source (no `time:` dependency), so this cannot be a Home Assistant `timestamp`-class sensor. Shows as unavailable until the first frame is received.
- **`<Entity Name> Exchange Failures`** (count): a cumulative count of outbound exchanges to this device (position/tilt/status/name requests) that received no valid response at all. Zero is a meaningful, always-published value here — it does not mean "unknown" the way it would for RSSI or Last Contact. A rising count on an otherwise-working device points at a marginal RF link (weak signal, interference, distance) worth investigating with the RSSI sensor above.
- RSSI and Exchange Failures update only when the hub actually processes a frame or exchange for that device — no background timers exist just to refresh them. Last Contact is the exception: it also republishes once a minute on its own heartbeat so it can count up while the device is quiet (see above).

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

## Key Extraction (Accept Foreign Pairing)

> **⚠️ Hardware-confirmed protocol, but not yet against a third-party hub.** A full extraction
> (0x28 through 0x33) between two boards — one running this responder, the other running this
> project's own pairing flow as the "hub" — recovered the hub's `node_id`/`system_key`
> byte-for-byte on real RF hardware. That confirms the crypto, state machine, and radio wiring are
> correct. It does **not** confirm compatibility with a genuine third-party hub (Somfy TaHoma/
> Smoove, Velux KLF200, etc.) — the discovery-response field guesses and IV-derivation assumption
> were reverse-engineered from this project's own encoder and a small number of captures, and a
> real hub's exact requirements may differ.

If you already own a working IO-Homecontrol installation (a hub plus paired devices) and want to
move it to this component, you normally need to extract that installation's `node_id`/
`system_key` — which otherwise requires resetting a device and sniffing a re-pair with an
external tool. This feature avoids that: it makes the ESP32 emulate an *unpaired device* so your
existing hub can pair to it directly, the same way it would pair to a real shutter. During that
pairing handshake your hub hands over its `node_id` and `system_key`; this feature recovers both
and prints a ready-to-paste YAML block, with no separate hardware and no device reset.

```yaml
home_io_control:
  # ... rst_pin / node_id / system_key / etc. as usual ...
  accept_foreign_pairing: true
```

Configuration variable:

- `accept_foreign_pairing` (Optional, boolean, default `false`): When `true`, dynamically creates
  the **"Accept Foreign Pairing (Key Extraction)"** switch entity, bound directly to this hub.

This lives directly under `home_io_control:`, alongside options like `tuning:` and
`exposed_senders:`. The generated switch always boots off (`restore_mode: ALWAYS_OFF`) so a
reboot can never leave it armed, and its name is fixed to "Accept Foreign Pairing (Key
Extraction)" (not configurable).

### Workflow

1. Flash the firmware with `accept_foreign_pairing: true` set in your `home_io_control:` block.
2. Turn the **"Accept Foreign Pairing (Key Extraction)"** switch on in Home Assistant. The hub
   arms for **10 minutes** and logs the throwaway node ID it will advertise.
3. Put your **existing** hub into its own "add device" / pairing mode, the same way you would to
   pair a new shutter to it.
4. Watch the ESPHome logs. On success, within a few seconds you will see a clearly-delimited
   block containing your installation's real `node_id` and `system_key`, ready to paste
   into a new `home_io_control:` block. The switch turns itself off immediately after a
   successful extraction.
5. If nothing happens within 10 minutes, the switch turns itself off and the log explains that no
   pairing attempt was seen (or, if a partial attempt was seen, which phase it reached — useful
   for diagnosing a missed frame, see the note below).
6. Copy the printed `node_id`/`system_key` into your new hub's YAML and reflash.

### Known limitations

- The recovered key is **not independently verified against your specific hub** by this feature —
  there is no automated read-back confirming it. Test it (e.g. by controlling a device) before
  relying on it; if it doesn't work, please file a GitHub issue with the (redacted, per the
  warning above) circumstances so the discovery-response format or IV-derivation assumptions can
  be corrected.
- On SX1262-based boards, a slow TX→RX turnaround can cause the responder to miss the hub's next
  frame right after transmitting a reply. The responder tolerates this by staying in its current
  state and waiting for the hub's own retry rather than assuming a single clean pass — if
  extraction seems stuck, give it a few more seconds before assuming failure. Confirmed working
  with SX1262 on both sides (as the *hub* and as the *responder*, the more demanding direction for
  this specific risk) — occasionally needing one automatic retry at the key-init or key-transfer
  step is expected and not a sign of failure.
- Only one extraction attempt is honored per 10-minute arm; the switch disarms itself immediately
  after the first successful extraction.
- Pairing timing generally — hop/dwell slicing, preamble selection, and discovery-window sizing —
  is not yet perfectly tuned across this project, and the key-extraction responder shares that
  same radio timing machinery with normal device pairing (`PairingEngine`). In practice this means
  discovery, key-init, or key-transfer can each need a retry before landing, on top of the
  turnaround-specific SX1262 behavior noted above. Extraction is still expected to succeed, but
  don't be surprised if it takes several hub-side retries (or, if the whole attempt times out, a
  fresh switch toggle) rather than a single clean pass on the first try.

## LR1121 Firmware Update

> **⚠️ Use at your own risk.** Flashing radio firmware can potentially brick the chip if it goes
> wrong. This project only ever touches the transceiver-firmware region (see below), which keeps
> every failure this feature can produce recoverable — but flash with the same caution you'd give
> any firmware update, and don't power-cycle the device mid-flash.

Semtech ships field-upgradable transceiver firmware for the LR1121, separately versioned from this
project. This feature flashes a Semtech-published image directly from your existing build — no
separate hardware, no vendor tooling — triggered by a Home Assistant button.

```yaml
home_io_control:
  radio_type: lr1121   # required
  busy_pin: 34          # required
  lr1121_firmware_update:
    source: github://Lora-net/radio_firmware_images/lr1121/transceiver/lr1121_transceiver_0103.bin
    # ref: optional branch/tag/commit; defaults to HEAD
    # checksum_md5: optional 32-hex-char hash; also the fallback when no `.bin.md5` sibling exists
    # target_version: optional hex (e.g. 0x0103); only needed when the filename carries no version
```

Configuration variables (all nested under `lr1121_firmware_update:`):

- `source` (Required): A `github://<owner>/<repo>/<path/to/file.bin>[@ref]` shorthand pointing at
  the **`.bin` file** itself. Fetched and MD5-verified once, at compile time — not a live
  "check for updates" mechanism, and no firmware binary is ever committed to this repo.
- `ref` (Optional): Branch, tag, or commit to fetch from. Defaults to `HEAD`, or an `@ref` embedded
  directly in `source`.
- `checksum_md5` (Optional): A 32-hex-character MD5 hash to verify the download against, used when
  no `<source>.md5` sidecar exists (or must agree with it, if one does).
- `target_version` (Optional): Overrides the firmware version this build believes it is flashing.
  Normally derived from the filename (`..._0104.bin` → `1.4`); only needed when a mirrored or
  renamed image's filename carries no version.

The block's mere presence is the build flag: adding it recompiles the flash button and a boot-time
bootloader-version read into the firmware; removing it takes them back out. There is no runtime
toggle — entering and leaving flash mode is a recompile + OTA each way. All of your normal
cover/light/switch/lock entities keep working throughout, unaffected.

### Which images can I flash?

This project currently flashes only the transceiver-firmware region, not the bootloader — the
bootloader-updater path is a categorically riskier operation (see [ADR 0020](https://github.com/laberning/home_io_control/blob/main/docs/adr/0020-flash-lr1121-transceiver-firmware-not-the-bootloader.md)) and isn't implemented yet. So what you
can reach depends on what your board's bootloader already is: the near-universal `0x2100`
bootloader can take firmware `1.1`–`1.3`, while `1.4` needs bootloader `0x2101` and is unreachable
until bootloader updates are supported.

Every boot reads the bootloader version (a free chip reset before anything else is configured) and
reports it, together with whether the configured `target_version` can proceed, in the startup
config dump:

```
LR1121 bootloader version: 0x2100
Firmware update target: 1.4 -- CANNOT PROCEED: needs bootloader 0x2101, this chip has 0x2100
```

A target version this build has no opinion on (something Semtech publishes later) is never
refused outright — it's routed through the two-press confirmation below instead, so the feature
keeps working on firmware that doesn't exist yet.

### The flash button and two-press confirmation

Configuring the block creates a **"Flash LR1121 Radio Firmware"** button, bound directly to the
hub. Pressing it:

- **Refuses immediately, without touching the chip**, if the configured target can't proceed
  (wrong bootloader) or the radio doesn't look like an LR1121 at all — the reason is the same one
  already printed in the boot-time config dump.
- **Asks for a second press** if the target isn't unambiguously newer than what's installed (already
  installed, unverified compatibility, or an unknown version). A second press within about a minute
  proceeds; letting the window expire cancels it.
- **Flashes immediately** if the target is a known-good, strictly newer version.

Once a flash starts, the chip is erased and rewritten, with progress logged periodically
(`Flashing LR1121: 1630/16304 words (10%)`) on both the serial console and the API connection — on
a `1.3` image this takes a few seconds total. The ESP32 reboots on its own once the sequence
finishes, including after a failure. **A failed erase or write is recoverable**: press the button
again after the reboot to retry.

## Device Type and Capability Notes

- **Cover-like families** (shutters, awnings, blinds, openers, curtains) are the primary supported path today. These support full position control (0–100%).
- **Light** support (binary and dimmable) has been validated against real hardware (a Somfy Izymo dimmer).
- **Lock and switch** support exists, but remains experimental and has not been validated against real hardware.
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
  radio_type: sx1276
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