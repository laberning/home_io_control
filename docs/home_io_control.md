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
- `accept_foreign_pairing` (Optional, default: `false`): Adds a "Recover System Key" switch entity for pulling a device's system key from another controller. See the Key Extraction section below.
- `diagnostic_probes` (Optional, default: `false`): Enables the `probe_device`/`probe_sweep` actions for sending opcodes this project hasn't fully decoded yet. See [Diagnostic probes](radio_diagnostics.md#diagnostic-probes).

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

## Grouping Entities into Home Assistant Devices

The cover, light, switch, lock, and button platforms accept ESPHome's own `device_id:` key to group an entity — and every companion entity it automatically generates — under a distinct Home Assistant device, instead of everything landing on the single physical ESPHome node. This is unrelated to `io_device_id:` (the IO-homecontrol protocol address below); `device_id:` is purely an ESPHome/Home Assistant UI-grouping concept.

```yaml
esphome:
  devices:
    - id: patio_awning_device
      name: "Patio Awning"

cover:
  - platform: home_io_control
    device_id: patio_awning_device
    id: patio_awning
    name: ""
    io_device_id: "FEEB1E"
    io_device_type: "awning"
```

- Declare each sub-device once under `esphome: devices:`, then reference its `id:` from any entity's `device_id:`.
- Every companion entity a platform generates (favorite/ventilation buttons, the silent-operation switch, the device-name/active-issue/RSSI/last-contact/exchange-failures sensors — see each platform section below) automatically inherits the same sub-device as its parent entity.
- **Naming caveat**: Home Assistant composes an entity's displayed name as `<device name> <entity name>`. Giving the sub-device and the entity the same name (e.g. both "Patio Awning") doubles up as "Patio Awning Patio Awning" — and every companion's own generated name (e.g. "... Favorite Position") gets that device-name prefix on top too. To show only the sub-device's name, give the entity `name: ""` (ESPHome's convention for "this entity is the device"), as in the example above — or the equivalent YAML literal `name: None`/`name: none`, which also selects this idiom but additionally requires `esphome: friendly_name:` to be set (matching ESPHome's own behavior everywhere else; `name: ""` needs no such setting). Either form requires an explicit `id:` as well: with an empty name there is nothing left to derive the companion entities' internal IDs from, so config validation fails with a clear error unless `id:` is given.
- Hub-level entities that are not tied to a single cover/light/switch/lock — the key-extraction/1W-key-adoption arming switches, the LR1121 firmware/bootloader controls, tuning numbers and selects, and each 1W controller identity's command buttons — do not currently support `device_id:` and always appear on the hub's main ESPHome device. The pairing button itself is not one of these: see the Button Platform section below, its `device_id:` (and its companion sensor's) work like any other device-bound platform.

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
- The favorite/ventilation buttons, the silent-operation switch (see below), and all of the diagnostic sensors above follow the cover's own `device_id:`, if one is set — see "Grouping Entities into Home Assistant Devices" above.
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

Beyond the entities generated from your `cover:`/`light:`/`lock:`/`switch:` YAML, Home IO Control exposes five **hub-level actions** through ESPHome's native API. These are one-off/advanced operations that would clutter the entity UI if they were always-visible buttons, so they're only reachable via Developer Tools or an automation.

| Action | What it does | `verified` can be `true`? |
|---|---|---|
| `rename_device` | Renames a paired actuator and reads the name back to confirm the write. | Yes |
| `identify_device` | Makes a device physically identify itself (brief jog/flash) so you can tell which physical motor a device ID belongs to. | No — no readback exists for a jog |
| `force_open_device` ⚠️ *experimental* | Requests a fully-open move at elevated protocol priority, intended to override wind/rain soft locks. Confirmed to move the device correctly; **not yet confirmed to actually override an active lock** — see the warning below. | No — the outcome is asynchronous |
| `scan_paired_devices` | Broadcasts a roll-call and reports every already-paired device that answers — no target `device_id`, no arguments at all. | No — nothing here is read back either |
| `oneway_set_position` | Sends a numeric position as a configured 1W controller identity. Takes `controller_id` and `position`, not a `device_id` — 1W addresses a device class. See the "Sending 1W Commands" section. | No — 1W has no reply at all |

Two more actions, `probe_device` and `probe_sweep`, exist behind a separate opt-in
(`diagnostic_probes: true`) for sending opcodes this project hasn't fully decoded yet — see
[Diagnostic probes](radio_diagnostics.md#diagnostic-probes) in the tuning guide.

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

Every action except `scan_paired_devices` takes `device_id`: the 6-hex-character IO-homecontrol device ID — the same value you set as `io_device_id` in the entity's YAML, and the same ID the pairing log prints. This is the protocol-level actuator ID, **not** the Home Assistant entity ID. `scan_paired_devices` takes no `data:` at all — it isn't aimed at one device.

### Result events

Every action fires the same Home Assistant event, `esphome.home_io_control_action_result`, so one automation trigger can react to any of them:

| Field | When present | Meaning |
|---|---|---|
| `action` | always | Action name, e.g. `rename_device`. |
| `device_id` | always | Target device ID — **empty** for `scan_paired_devices`, which has no single target. |
| `success` | always | Whether the action succeeded. For `scan_paired_devices`, `true` whenever the broadcast went out — a scan that heard nothing back is a successful scan, not a failure. |
| `verified` | always | Whether a follow-up readback confirmed the result — see the table above for which actions can ever set this `true`. |
| `message` | always | Human-readable outcome summary. For `scan_paired_devices` this is the full multi-line report (see below), not a one-line summary. |
| `requested_name`, `applied_name` | `rename_device` only | Requested vs. verified device name. |
| `result_code`, `result_code_name` | `rename_device`, `identify_device`, and `probe_device`, when the device replies `CMD_ERROR_RESP` | Decoded protocol result code. |
| `probe`, `index` | `probe_device` and `probe_sweep` only | Probe name and the requested index (or swept range). |
| `response_cmd`, `response_cmd_name`, `response_hex` | `probe_device` only — a sweep's per-index replies are in `message` | The reply's command byte, decoded command name, and full raw wire hex. |

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

### `scan_paired_devices`

No fields — call it with an empty `data:` (or omit `data:` entirely). Like the other actions it
becomes `esphome.<node_name>_scan_paired_devices`; for the sample V2 config
(`name: hioc-heltec-v2`) that's `esphome.hioc_heltec_v2_scan_paired_devices`.

Broadcasts a `CMD_DISCOVER_SPE_REQ` (0x2A) roll-call and listens for replies from **every
device that already holds this hub's system key** — not just the ones you have YAML entities
for. Each responder is looked up against the hub's device registry and grouped into a `Known:`
section and an `Unknown:` section; unknown responders additionally get a lead-in line and a
ready-to-paste YAML block, the same one a successful pairing prints.

```yaml
action: esphome.hioc_heltec_v2_scan_paired_devices
```

A realistic report, one already-configured device and one that isn't:

```
Roll-call: 2 devices detected (1 known, 1 unknown)
Known:
  30E1F2: horizontal_awning subtype=0 rssi=-52dBm manufacturer=Somfy turnaround=40s power_save=always_alive [known]
Unknown:
  415CE4: light subtype=0 rssi=-61dBm manufacturer=Somfy turnaround=40s power_save=always_alive [unknown]
    Paste this into your YAML to register it:
  light:
  - platform: home_io_control
    name: "My Device"
    io_device_id: "415CE4"
    io_device_type: "light"
    io_subtype: 0
```

A single scan reports at most **24 devices**. If more than that answer, the report says so
explicitly with a `NOTE: more than 24 devices answered; the list below is truncated.` line rather
than quietly listing a subset — so a large install can never look like devices have gone missing.
Raising the limit means changing `SCAN_MAX_REPLIES` in `management_actions.cpp`; each additional
slot costs 12 bytes of stack, so there is plenty of headroom if you need it.

Known devices are always listed before unknown ones, and either the `Known:` or `Unknown:`
section is omitted entirely when it would be empty (e.g. a scan where every responder is already
known prints no `Unknown:` header at all).

`success` is `true` whenever the broadcast went out, **including when nothing answers** — a
scan that hears nothing is a valid result, not a failure. `device_id` on the result event is
always empty (there is no single target); the full report above is the event's `message`.

**A single scan may still miss devices — run it again if one you expect is absent.** Paired
devices duty-cycle across the three radio channels independently of the hub, so the hub retries
the broadcast on all three IO-homecontrol channels (CH2, then CH1, then CH3), each with its own
full `pairing_discovery_wait_ms` listen window, before returning the merged report. A past hub-side
listening bug used to cause most misses; that has since been fixed, so a missed reply is now
genuinely uncommon, but it is still possible for a device to be missed through all three attempts.
This makes a single scan take a while: three full-length windows plus transmit time, roughly `3 ×
pairing_discovery_wait_ms` (~6 seconds at the 2000 ms default). It will also log an ESPHome
"operation took a long time" warning on every run — a known, accepted tradeoff. That warning stops
repeating for anything that blocks under ~2.5 s, and a shorter window was tried for exactly that
reason; it was reverted because it made devices' replies land just after the window closed, where
they are dropped, so scans started missing devices. A recurring log line is the lesser problem. If
a device you know is paired doesn't show up, just trigger the action again — it costs nothing
else, since the action has no side effects to worry about repeating.

**This cannot help you pair a new device.** A device only answers 0x2A if it already holds
this hub's system key — a device sitting in learning mode, waiting to be paired, holds no key
yet and stays silent. See `docs/radio_diagnostics.md`'s `pairing_discovery_commands` section for
why 0x2A is deliberately excluded from the pairing discovery command list. Use the "Discover &
Pair" button for pairing a new device; use `scan_paired_devices` to check in on devices you have
already paired.

An unknown responder in the report is not, on its own, a sign of an intruder — it almost always
means a device you paired earlier whose YAML entry never got saved (or got lost), not a foreign
controller. The reply itself proves nothing more than "this device once received your system
key"; see the roll-call's protocol notes for why the reply carries no per-transaction proof.

### `probe_device` / `probe_sweep`

Fields: `device_id` (required), `probe` (required probe name), `index` (`probe_device`) or
`first_index` + `last_index` (`probe_sweep`).

Behind the separate `diagnostic_probes: true` opt-in — sends opcodes this project has observed on
the wire but not fully decoded, and reports the raw reply rather than an interpretation:
`probe_device` in the structured `response_cmd`/`response_cmd_name`/`response_hex` event fields, a
sweep's per-index replies inline in `message` instead. See [Diagnostic
probes](radio_diagnostics.md#diagnostic-probes) for the available probe names, argument shapes,
worked examples, and safety notes.

```yaml
action: esphome.hioc_heltec_v2_probe_device
data:
  device_id: "FEEB1E"
  probe: "private_fn"
  index: "0x06"
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
- All of the diagnostic sensors above follow the light's own `device_id:`, if one is set — see "Grouping Entities into Home Assistant Devices" above.

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
- All of the diagnostic sensors above follow the lock's own `device_id:`, if one is set — see "Grouping Entities into Home Assistant Devices" above.

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
- All of the diagnostic sensors above follow the switch's own `device_id:`, if one is set — see "Grouping Entities into Home Assistant Devices" above.

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
- The companion "Last Pairing Result" diagnostic sensor (see "Diagnosing a failed pairing attempt" below) follows the button's own `device_id:`, if one is set — see "Grouping Entities into Home Assistant Devices" above.

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
**"Last Pairing Result"** diagnostic text sensor — no YAML configuration needed. It follows the
button's own `device_id:`, if one is set (see "Grouping Entities into Home Assistant Devices"
above). It updates after every "Discover & Pair" attempt with a frozen, machine-readable summary:

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
  the **"Recover System Key"** switch entity, bound directly to this hub.

This lives directly under `home_io_control:`, alongside options like `tuning:` and
`exposed_senders:`. The generated switch always boots off (`restore_mode: ALWAYS_OFF`) so a
reboot can never leave it armed, and its name is fixed to "Accept Foreign Pairing (Key
Extraction)" (not configurable).

### Workflow

1. Flash the firmware with `accept_foreign_pairing: true` set in your `home_io_control:` block.
2. Turn the **"Recover System Key"** switch on in Home Assistant. The hub
   arms for **10 minutes** and logs the throwaway node ID it will advertise.
3. Put your **existing** hub into its own "add device" / pairing mode, the same way you would to
   pair a new shutter to it.
4. Watch the ESPHome logs. On success, within a few seconds you will see a clearly-delimited
   block containing your installation's real `node_id` and `system_key`, ready to paste into a new
   `home_io_control:` block. Some hubs (Velux KLR200 confirmed) don't stop there: after the key
   exchange they verify the address they were just handed with a follow-up request/challenge
   round, so the switch stays on — listening, not stuck — for up to one more minute after the key
   is printed rather than turning off immediately. Leave it on until it turns off on its own; a hub
   that never sends this follow-up simply leaves the extra minute unused — not observed in the one
   Velux KIG300 capture available, which itself ends at the key-confirm step and so has no
   post-key-exchange observation window to establish that hub sends nothing further.
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
- The address-verification round (step 4's follow-up request/challenge, `CMD_ADDRESS_REQ`/
  `CMD_ADDRESS_RESP`/`CMD_CHALLENGE_REQ`/`CMD_CHALLENGE_RESP`) is implemented from a single
  third-party capture (a Velux KLR200 pairing session) and has not been exercised against any real
  hub by this project — unlike the key exchange it follows, which has separately been
  hardware-confirmed as described in the warning above. That capture also shows the real device
  reporting a persistent identity in its `CMD_ADDRESS_RESP` distinct from its own node/session
  address — a distinction the io-homecontrol wire format appears to track generally, not something
  specific to that one device. This feature's emulated device only ever has one identity to offer
  per arm cycle, so it reports that same identity for both; whether any real hub requires (or even
  inspects) a genuinely separate value here is unconfirmed.
- On SX1262-based boards, a slow TX→RX turnaround can cause the responder to miss the hub's next
  frame right after transmitting a reply. The responder tolerates this by staying in its current
  state and waiting for the hub's own retry rather than assuming a single clean pass — if
  extraction seems stuck, give it a few more seconds before assuming failure. Confirmed working
  with SX1262 on both sides (as the *hub* and as the *responder*, the more demanding direction for
  this specific risk) — occasionally needing one automatic retry at the key-init or key-transfer
  step is expected and not a sign of failure. While an attempt is in progress (any state past the
  initial arm) the responder also holds the request channel instead of running its normal
  background channel scan — and defers its own background status polls, for the same reason a
  linked remote's press does — so it doesn't itself contribute to a missed reply by having hopped
  away or being mid-exchange with a device. Before the key is recovered, that hold releases on its
  own after a few seconds of no further progress from the hub, so a stray or abandoned attempt from
  another hub in range can't pin the responder to one channel for the rest of the 10-minute arm
  window. After the key is recovered, the hold is bound to the (longer) post-extraction grace window
  instead — see the next bullet — so it can legitimately last up to a minute or more while your hub
  runs its address-verification round, re-arming on every sign of progress from it.
- The switch does not disarm the instant the key is recovered — it stays armed for up to one more
  minute in case your hub follows up with an address-verification round (see the workflow above).
  A second (or repeated) extraction attempt from the *same* hub, inside that same arm window or
  after it, is honored too: a fresh discovery request from it starts a brand-new attempt rather
  than being silently ignored, so you can retry without toggling the switch off and back on in
  between. A different hub's discovery request during that window is still ignored, so it can't
  interrupt an address-verification round in progress with the hub you actually extracted from.
- This feature does not tell the foreign hub that the extraction happened. After a successful run
  (and especially after an address-verification round, which hands the hub a backbone address it
  then treats as confirmed) your existing hub believes it paired a new device and may show it as
  such in its own UI. That device will never answer again; there is currently no way to make this
  component un-enroll or otherwise clean up that phantom entry from your hub's side.
- Pairing timing generally — hop/dwell slicing, preamble selection, and discovery-window sizing —
  is not yet perfectly tuned across this project, and the key-extraction responder shares that
  same radio timing machinery with normal device pairing (`PairingEngine`). Three things keep the
  key-exchange step landing reliably within the first one or two tries instead of needing several:
  the discovery reply (0x29, the one frame a hopping listener has to catch cold) uses its own
  tunable preamble long enough to be reliably caught — see `cold_broadcast_reply_preamble` in
  [radio_diagnostics.md](radio_diagnostics.md); the responder holds the request channel instead of
  idle-hopping away mid-attempt (the bullet above); and the responder's 3-channel broadcast sends
  the request channel *last*, so a peer that reacts the instant it hears that channel doesn't fire
  its next frame while the responder is still transmitting a later leg. Occasional retries are
  still expected and not a sign of failure; if the whole attempt times out, retry it (a second
  attempt works without a switch toggle, per the bullet above).

## Recovering a 1W Controller Key

> **⚠️ Receive-only, but it recovers a real secret.** This feature never transmits anything. It
> listens for a frame a 1W remote broadcasts during its key-copy gesture and decrypts it. The
> decryption needs no secret of its own — see the security note below — so treat the recovered key
> exactly as you would treat your `system_key`.

One-way (1W) installations — a handheld remote driving a shutter or awning directly, with no hub —
have their own network key. This feature recovers it by overhearing a single `0x30` "add
controller" broadcast, which is what a 1W remote sends while its **remote-to-remote key-copy mode**
is active in order to hand its network key to a new remote.

**For controlling a device, you almost never need this.** A device accepts whichever key arrives
during its own association-mode window — enrolling this hub as a controller (see "Sending 1W
Commands" below) works just as well with a freshly generated key as with a recovered one, so there
is nothing to gain from recovering a key just to enroll with it. Recover a key only when you
specifically need to **become** an existing, already-registered identity rather than add a new one:

- **Replacing a remote that is gone for good** (lost, broken, retired), so the hub can take over
  its exact identity instead of the device learning a new one.
- **Understanding or documenting an existing network** — a diagnostic use, not a control one.

**Never reuse a recovered key under the original remote's address while that remote is still in
use.** A device tracks one rolling-sequence high-water mark per source address, fed by whichever
transmitter used it most recently. Two independent transmitters sharing one address — the original
remote and this hub — cannot coordinate that counter: the hub's own persisted value only reflects
what *it* has sent, so its next frame is very likely to land at or below what the device already
accepted from the real remote and be silently rejected as a replay. Reuse an identity's address
only once its original transmitter will never transmit again.

```yaml
home_io_control:
  # ... rst_pin / node_id / system_key / etc. as usual ...
  recover_oneway_key: true
```

Configuration variable:

- `recover_oneway_key` (Optional, boolean, default `false`): When `true`, dynamically creates the
  **"Recover 1W Controller Key"** switch entity, bound directly to this hub.

Like `accept_foreign_pairing`, this lives directly under `home_io_control:`, the generated switch
always boots off (`restore_mode: ALWAYS_OFF`) so a reboot can never leave it armed, and its name is
fixed (not configurable). The two features are independent — arming one never arms the other.

### Workflow

1. Flash the firmware with `recover_oneway_key: true` set in your `home_io_control:` block.
2. Turn the **"Recover 1W Controller Key"** switch on in Home Assistant. The hub arms for
   **10 minutes** and logs that it is listening. Nothing is transmitted.
3. Trigger the **key-copy gesture on your existing 1W remote** — the remote-to-remote copy mode
   described in its manual, the one you would use to teach a second remote the same network.
   Do this near the hub.
4. Watch the ESPHome logs. On success you will see a clearly-delimited block containing the
   recovered key and a ready-to-paste `oneway_controllers:` entry. The switch turns itself off
   immediately — one adoption per arm.
5. Paste the block directly into your `home_io_control:` block — the recovered key is already
   inline (`system_key: "..."`), same as the 2W Key Extraction report — and reflash.
6. If nothing happens within 10 minutes, the switch turns itself off and says so. Re-arm and try
   again closer to the device.

### Reading the result

The report tells you the **MAC status**, which is your on-the-spot evidence that the recovered key
is correct:

- **`MAC VERIFIED`** — the frame carried an authenticator and it checked out *under the key that
  was just recovered*. This is the strongest confirmation available without commanding a device.
- **`MAC FAILED`** — the authenticator did not check out. The key is probably wrong (a marginal
  reception is the usual cause). Re-arm and repeat the gesture closer to the hub.
- **`MAC not present`** — the frame carried no authenticator to check. Not an error; real hardware
  frequently omits it. The key may still be correct — enroll with it (see "Sending 1W Commands")
  and confirm by actually commanding the device, which is now possible.

Two fields in the emitted block deserve a note:

- **`node_id` is deliberately absent.** The hub transmits under its *own* address, derived from
  your hub's `node_id`. It must never impersonate the existing remote: reusing that address would
  hijack the remote's rolling sequence counter and break it.
- **`io_device_type` is prefilled from what was overheard**, if this hub happened to see other 1W
  traffic from the same remote while armed. It is a well-founded guess, not authoritative — verify
  it. If nothing was observed, the line is commented out with a pointer to the DEBUG log line that
  reveals it.

### Security note

Anyone within radio range of a key-copy gesture can recover the network key this way. The wrapped
key in that broadcast is protected only by a **publicly-known transfer key**, using an
initialisation vector derived from the sender's own address — which is in the same frame's header,
in plaintext. There is no secret involved in the unwrap.

That is a property of io-homecontrol, not something this project introduces; the same framing
applies as to the Key Extraction section above. The practical advice
is the same as for any secret: perform the key copy **once**, indoors, and treat the recovered key
as the credential it is. Raw `0x30` payloads are masked in this component's own frame logs for
that reason — the recovered key is printed in exactly one deliberate place, the adoption report.

## Sending 1W Commands (Controller Identities)

The hub can act as a **1W controller** — the kind of thing a wall remote is — and drive devices by
transmitting. This is off unless you configure it, and it signs with a key you already hold: the
same authorisation as any 2W command this component sends.

> **⚠️ Read this before configuring: a device only obeys a controller it has been taught.**
> Real-hardware testing established that 1W actuators keep a **table of registered controllers**.
> A frame that is correctly built, correctly addressed and signed with a key the device accepts is
> still ignored if this hub's source address is not in that table. **Enrollment (below) is what
> registers it** — do that first, for every identity, before expecting any command to move a device.

### What 1W is, and what it is not

Everything about this feature follows from two properties of the protocol:

- **A 1W command addresses a device *class*, not a device.** There is no unicast form. A command
  sent as a `roller_shutter` identity reaches every roller shutter in range that holds the signing
  key. That is what 1W *is*, not a limitation to design around.
- **Nothing replies.** No acknowledgement, no status, no error. A command a device ignored is
  indistinguishable on the radio from one it obeyed.

The second one shapes the whole feature. There is no failure you can be notified about, so the
"Last 1W Command" sensor and the section below are the diagnostic tools.

### Controller identities

Because nothing on the wire names a device, what distinguishes one 1W control surface from another
is the *controller* doing the transmitting. That triple — source address, network key, device class
— is a **controller identity**, and it takes the place node addressing has for 2W (ADR 0027).

**Minimal example** — the fewest fields that generate a working set of buttons:

```yaml
home_io_control:
  # ... radio pins, node_id, system_key ...
  oneway_controllers:
    - id: velux_windows
      io_device_type: window_opener
      commands: [open, close, stop]
```

**Full example** — every optional key at once, so you can see the whole shape in one place:

```yaml
home_io_control:
  oneway_controllers:
    - id: velux_windows
      io_device_type: window_opener                    # see "Named device types" below
      commands: [open, close, stop, favorite]
      node_id: A11CE0                                   # optional -- overrides the derived address
      system_key: FEDCBA98765432100123456789ABCDEF      # optional -- reuse a recovered network's key
      initial_sequence: 4000                            # optional -- seed the rolling counter
      manufacturer: somfy                               # required only because enrollment: true, below
      enrollment: true                                  # generates the "Enroll 1W Controller" button
      enrollment_with_mac: false                        # optional -- see the table below
```

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | Handle the generated entities are named and ID'd from. |
| `io_device_type` | yes | The device class this identity commands — see the "Named device types" table under Device Type and Capability Notes below for the full list. |
| `node_id` | no | Source address to transmit as. **Derived from your hub's `node_id` and this `id` when omitted**, deterministically at *compile* time — a derived address takes part in the same collision checks as an explicit one (a clash fails the build), and is printed at boot marked `(derived)`, since nothing in your YAML shows it otherwise. Asking you to invent a 3-byte radio address instead would be an unanswerable question: nothing tells you which addresses are safe, and colliding with a real remote in range silently desyncs both transmitters' counters. |
| `system_key` | no | Network key for this identity. **Defaults to the hub's own** — every new identity works fine with the default. Only set this to a recovered key when reusing an existing, already-registered identity (see "Recovering a 1W Controller Key"). |
| `initial_sequence` | no | Seeds the rolling counter. The day-one remedy for a desynced device — see troubleshooting. |
| `commands` | no | Which buttons to generate: `open`, `close`, `stop`, `vent`, `favorite`. `stop` is pinned by a published reference vector; `vent` matches the reference remote's source but is unconfirmed by any capture; `favorite` is extrapolated with no reference support and is directly contradicted by this project's own capture of a real My/favorite button press, which encodes it a different way entirely — see `create_1w_execute_command()` in proto_commands.h. Treat `favorite` as untested. |
| `manufacturer` | conditional | The manufacturer ID byte an enrollment frame carries on air — a named value such as `somfy`, or a raw integer; see the "Named manufacturers" table under Device Type and Capability Notes below for the full list. **Required whenever `enrollment: true` is set** — the build fails otherwise, rather than silently broadcasting `0`. Find the value from a "Recovering a 1W Controller Key" report for this network, or the device's own documentation. |
| `enrollment` | no | Build flag (default `false`) for this identity's **"Enroll 1W Controller"** button — see "Enrolling this hub as a controller" below. |
| `enrollment_with_mac` | no | Whether the `0x30` half of the enroll button's press carries a trailing MAC (default `false`, meaning **no MAC at all** — there is no in-band form for this frame, see below). Real hardware disagrees on this byte: most captures this project holds carry no MAC (the default), but a real Somfy Izymo has separately been shown to accept the MAC-bearing form too. Untested manufacturers may need either — try flipping this before assuming enrollment doesn't work at all. |

There is no 1W `force_open` button. The only wire byte this project ever associated with a
"force open" label, `0x64`, decodes to an ordinary numeric position (50%) when tested against real
hardware as an outbound command — see the "Numeric positions" section below. The hub's
force-open (`force_open_device`) is a separate, 2W, per-device action; see that section above.

### Enrolling this hub as a controller

**This is the step that makes every command above actually move something.** A 1W device ignores
any frame from a source it has not been taught, no matter how correctly it is built or signed —
see the warning at the top of this section. Enrollment is what teaches it.

Add `manufacturer:` and `enrollment: true` to an identity — see the full example above. `enrollment:
true` is the build flag: its presence is the whole gate, and adding or removing the line and
reflashing is the feature's entire lifecycle. Setting it creates one more entity for that identity:
**"\<Identity\> Enroll 1W Controller"**, `entity_category: config`.

**Enrolling is additive, not destructive.** A device's controller table holds more than one entry,
each with its own key — enrolling this hub alongside an already-registered remote leaves that
remote working exactly as before; the device answers commands from either.

**The gesture is two-sided, and only one half is a button press.**

| Half | Who does it | What it is |
|---|---|---|
| Receiver enters association mode | **you, physically** | **2 second** hold on the actuator's PROG button, confirmed by its own indicator |
| Controller offers its credential | **the hub** | one **short** press of the "Enroll 1W Controller" entity |

Get the order and the durations right: 2 seconds on the receiver, *then* one press on the hub — not
the other way around, and not a long hold on the hub's entity (there is nothing to hold; a press is
a press). Getting the two halves' timing backwards is the most common failure mode here, not a
protocol problem.

The press sends two bursts back to back — `0x39` (self-directed; carries only this identity's own
address, so it can only ever clear its own prior entry, never a different controller's),
immediately followed by `0x30` (the credential itself) — each ~125 ms, ~4 copies, no gap beyond the
bursts' own airtime. This is the documented 1W pairing handshake (a real Somfy Smoove remote does
exactly this, corpus-captured), not two independent actions. There is no "learn window" on the
hub's side beyond that, because the device owns its own timeout and there is nothing further to
wait for. **Only one device should be in association mode at a time**: the frames reach every
device of that class in range that is currently listening, so a second actuator in learn mode
nearby would be taught too.

The `0x30` half's MAC trailer is controlled by `enrollment_with_mac:` (default `false`, no MAC at
all) — see the option table above if enrollment doesn't take with the default shape.

**A hub cannot enroll into a device nobody has walked up to.** The receiver's physical PROG hold is
the real safety interlock here, stronger than any software confirmation could be — it is why this
feature has no separate arming switch the way the (irreversible) LR1121 bootloader rewrite does.

**Un-enrolling without re-enrolling** is reached through its own explicitly-named action — the
Enroll button's `0x39` above always re-registers via the `0x30` that follows it, so this is the
path when you want the removal without the re-add:

```yaml
- action: esphome.<device_name>_oneway_remove_controller
  data:
    controller_id: velux_windows
```

This sends `0x39` alone, nothing else. It carries only this identity's own source address, so it
cannot remove a different remote's registration — the same property that makes the Enroll button's
`0x39` prelude safe to send automatically.

> **⚠️ Un-enrollment is unconfirmed on real hardware.** This action has not been shown to have any
> effect on real hardware — the hub keeps controlling the device afterwards regardless. The most
> likely explanation, by analogy with enrollment itself, is that a device only acts on `0x39`
> while its receiver is in the same **2 second PROG association mode** enrollment needs. Treat
> "un-enroll" as the documented design intent, not a confirmed rollback, until this is retested
> with that gesture.

**When you are done enrolling**, remove `enrollment: true` from the identity and reflash. A build
that can put a device into someone else's controller table should not be the build that runs
permanently — the same reasoning as removing `recover_oneway_key: true` after a key recovery.

### Generated buttons

Each name in `commands:` generates a button. Their **IDs follow `<identity_id>_<command>`** —
`velux_windows` + `open` → `velux_windows_open` — and that rule is a documented contract, because
you cannot compose against IDs you cannot predict. Entity names derive from the same pair
("Velux Windows Open").

These are created from the `oneway_controllers:` block rather than declared as
`button: - platform: home_io_control` entries, deliberately. A platform entry would have to decide
what the button *is* from the presence of some key, which is how a device-bound switch that merely
forgot its `io_device_id` once became the security-sensitive one instead of failing validation.
Creating these from the hub block makes that class of mistake structurally impossible.

### Continuous control: composing sliders from buttons

**1W support is still very early: there currently is no 1W cover entity. What
this component gives you natively is buttons and the `oneway_set_position` action below. But you
can build a slider on your own with ESPHome:

**Covers: `time_based`.** ESPHome's `time_based` cover platform already does exactly this, with
asymmetric durations and endstop handling, and it is a standalone platform rather than a mixin —
so it composes rather than inherits:

```yaml
cover:
  - platform: time_based
    name: "Roof Window"
    open_action:  {then: [button.press: velux_windows_open]}
    close_action: {then: [button.press: velux_windows_close]}
    stop_action:  {then: [button.press: velux_windows_stop]}
    open_duration: 30s
    close_duration: 28s
    has_built_in_endstop: true
```

Note the estimate is exactly that: pressing the *physical* remote moves the device without telling
Home Assistant, so the position drifts until the next full open or close re-synchronises it.

### Numeric positions

For anything other than the generated buttons there is an action (ADR 0006):

```yaml
- action: esphome.<device_name>_oneway_set_position
  data:
    controller_id: velux_windows
    position: "40"
```

`position` runs 0 (fully open) to 100 (fully closed) and is passed as a string, like every argument
on this component's action surface.

The result event reports only that the command was **queued**. Nothing downstream can ever upgrade
that to "the device moved".

### The "Last 1W Command" sensor

Every identity gets one, and it is the only feedback this feature can produce:

```
CLOSE -> window_opener seq 4013
```

It reports what the hub **transmitted** — never that a device acted, because that is not knowable.
It also shows the **sequence** used, which is the number you need for the troubleshooting below.

### Troubleshooting

With no reply frames, this ladder is the diagnostic. Work down it in order.

**Nothing appears in "Last 1W Command" after a press.** The command never reached the transmitter.
Check that the button you pressed belongs to the identity you think it does (`<identity_id>_<command>`),
and look for `no controller identity` in the log.

**The sensor says `not sent (no sequence reserved)`.** The identity resolved but its counter could
not be written to flash, so nothing was built — the hub refuses to transmit a sequence it has not
durably reserved, because reusing one is unrecoverable. This is a storage failure, not a radio one.

**The sensor updates but the device does not move.** In order of likelihood:

1. **The device does not know this controller.** A 1W device obeys only source addresses it has
   been *taught* — see "Enrolling this hub as a controller" above. This is established on real
   hardware, not inferred: devices holding the exact key the hub signed with, addressed correctly,
   did not react until the hub's own address was actually enrolled into them. **Enroll this
   identity first**, following the two-sided gesture exactly (durations matter), before working
   through the rest of this list.
2. **Key mismatch.** The identity's `system_key` no longer matches what this address enrolled with
   — most often because `system_key` was changed after enrollment, or an identity meant to reuse an
   existing network's key (see "Recovering a 1W Controller Key") was never actually re-enrolled
   with that key. Re-run enrollment for this identity after any `system_key` change.
3. **Wrong device class.** `io_device_type` selects the broadcast address. A shutter will not act
   on a command addressed to the awning class. Compare against the class you see in overheard 1W
   traffic from the real remote.
4. **Desynced counter.** The device remembers the highest sequence it accepted from you and rejects
   anything at or below it. If your counter fell behind — a replaced board, a restored backup — every
   command is silently dropped. **Remedy:** raise `initial_sequence:` above the value in the sensor
   and reflash. Move it in *small* steps: devices accept a forward jump only within a window
   (~1000), so overshooting fails exactly like undershooting.

### What is stored on the device

The rolling sequence counter, and nothing else. It is the one exception to this component's
otherwise absolute rule that YAML is the only source of truth, because the counter is neither
configuration nor re-learnable from the air — a 1W device never transmits, so nothing reports the
high-water mark your counter has to stay ahead of. ADR 0025 records the exception and its cost;
the practical consequence is that **replacing your board loses the counters**, and the identities
on the new board will need `initial_sequence:` raised once.

## LR1121 Firmware Update

> **⚠️ Give this the same care as any firmware update.** Make sure the device is on stable mains
> power that nobody is going to unplug, and don't power-cycle it while a flash is running. Most of
> what's described here is recoverable if it goes wrong; one part of it is not, and that part is
> clearly marked.

### What's actually on the radio chip

The LR1121 is a separate chip from the ESP32, and it runs its own software — nothing to do with
this ESPHome component or the firmware you flash over USB/OTA. That software comes in two pieces,
and it helps to know which is which before you update anything:

- **Transceiver firmware** — the radio program itself. This is what talks io-homecontrol. Semtech
  versions it `1.1`, `1.2`, `1.3`, `1.4`, … and publishes each release as a `.bin` file.
- **Bootloader** — a much smaller program that runs first, checks the transceiver firmware is valid,
  and starts it. Crucially, the bootloader is *also* what makes replacing the transceiver firmware
  possible at all. It has its own version numbers: `0x2100`, `0x2101`.

Both come from [Semtech's image repository](https://github.com/Lora-net/radio_firmware_images/tree/master/lr1121),
and this component downloads whichever you point it at when you build — nothing is bundled here.

**The two are linked:** a newer transceiver firmware can require a newer bootloader underneath it.
Today that matters in exactly one place — firmware `1.4` requires bootloader `0x2101`, while `1.1`
through `1.3` run on `0x2100`. Most boards shipped with `0x2100`, so reaching `1.4` means updating
the bootloader first. Your device reports both versions in its startup log, so you never have to
guess which situation you're in.

### How risky is each one?

| Updating | Risk | If it fails |
|---|---|---|
| **Transceiver firmware** | Low | Recoverable. The bootloader is untouched, so the chip can always be reached again — press the button once more and re-flash. Worst case the radio is silent until you do. |
| **Bootloader** | **High — not reversible** | The bootloader is what makes recovery possible. If a bootloader write is interrupted, there is nothing left to recover *with*, and this project cannot fix the chip. |

So: updating transceiver firmware is routine and safe to try. Updating the bootloader is a
deliberate, one-way operation that you opt into separately, and it is gated behind its own switch so
you cannot trigger it by accident. Both are described below.

Whichever you do, the single most useful precaution is boring: **stable power throughout.** A flash
blocks the device for a few seconds and it will go briefly unavailable in Home Assistant — that's
normal, and reaching for the plug at that moment is the one reliable way to turn a safe operation
into a broken chip.

Everything on this page has been run end to end on real hardware (a LilyGO T3-S3), including the
bootloader update.

### Flashing transceiver firmware

Triggered from a Home Assistant button, straight from your existing build — no extra hardware, no
vendor tooling.

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

Without the `bootloader:` sub-block below, the answer depends on the bootloader your board already
has. On the common `0x2100` that means firmware `1.1` through `1.3`; `1.4` needs `0x2101` and stays
out of reach until you deliberately opt into the bootloader update.

You don't have to look this up. Every boot reads the bootloader version — a free chip reset, before
anything else is configured — and prints it alongside a verdict for whatever you configured:

```
LR1121 bootloader version: 0x2100
Firmware update target: 1.4 -- CANNOT PROCEED: needs bootloader 0x2101, this chip has 0x2100 -- add a bootloader: sub-block to lr1121_firmware_update: to enable the (irreversible) upgrade path
```

That verdict is computed once at startup, so pressing the button when it can't succeed costs
nothing — it refuses immediately without touching the radio.

If you point `source:` at a firmware version newer than this component knows about, it is **not**
refused: this build's compatibility table is a point-in-time snapshot, not an authority, so an
unfamiliar version is routed through the two-press confirmation instead. The feature keeps working
on releases that didn't exist when it was written.

### Updating the bootloader (irreversible — opt in deliberately)

> **⚠️ This one is a one-way door.** A failed bootloader write has **no recovery path in this
> project** — see the risk table above for why. Mains power only, and don't interrupt it. Read
> [ADR 0021](https://github.com/laberning/home_io_control/blob/main/docs/adr/0021-flash-the-lr1121-bootloader-behind-an-arming-switch.md)
> before configuring this.

**Why you might want to.** Semtech's advisory SEM-PSA-2026-001 lists three CVEs affecting the
LR1121, and names bootloader `0x2101` as a threshold alongside firmware `1.4`. The most severe of
them is a weakness in secure boot — which is the bootloader's own job — so updating only the
transceiver firmware most likely does not close it. Worth knowing before you rush: all three
require **physical access to the chip's SPI pins**, so this is defence in depth for a hub sitting
inside your house, not an emergency.

**What actually happens.** It's three steps, not one, and the radio has no working firmware in
between: a small *loader* image is written first, that loader rewrites the bootloader, and then your
chosen transceiver image is written on top. The middle step is the one that cannot be retried. The
whole sequence takes about 10 seconds, during which the device is unresponsive.

Opt in with a nested `bootloader:` block, pointing at the matching **loader** image (not a
transceiver image) Semtech publishes alongside the transceiver firmware:

```yaml
home_io_control:
  radio_type: lr1121   # required
  busy_pin: 34          # required
  lr1121_firmware_update:
    source: github://Lora-net/radio_firmware_images/lr1121/transceiver/lr1121_transceiver_0104.bin
    bootloader:
      source: github://Lora-net/radio_firmware_images/lr1121/loader/lr1121_loader_2100.bin
      # ref / checksum_md5: same meaning as the outer block's
```

Pairing this block with an outer `source:` that's known to require the *existing* bootloader
`0x2100` is a build-time error, not merely discouraged: after the rewrite that image would be
unflashable, so ESPHome refuses to compile the config at all rather than let it arm a trap. This
guarantees the recovery transceiver image is already in ESP32 flash before anything is erased, so a
config mistake fails the build rather than bricking a board.

If the outer `source:` names a firmware version this build's compatibility table has never heard of
(the table's advisory, not exhaustive — see the "Which images can I flash?" section above),
that's only a build-time **warning**, not an error: the build will not gamble an irreversible write
on a requirement it doesn't know, so the `bootloader:` block is accepted but the rewrite path stays
inert at runtime (the boot-time log explains why, and the flash button refuses without touching the
chip). Extending the compatibility table once Semtech publishes the pairing is a one-line edit — see
`LR1121_KNOWN_BOOTLOADER_REQUIREMENTS` in `lr1121_firmware_decisions.h`.

Configuring the block also creates an **"Allow LR1121 Bootloader Rewrite (Irreversible)"** switch
on the hub, defaulting **off** and never auto-arming after a reboot. The existing flash button
performs the three-stage rewrite only while that switch is on **and** the cached verdict says the
upgrade applies; with the switch off, a press refuses immediately without touching the chip. There
is no separate two-press confirmation for this path — the switch **is** the confirmation, and it's
visible in Home Assistant so "is this armed?" is answerable by looking rather than remembering.

A completed (or failed) rewrite always ends in an ESP32 reboot, which clears the switch again — the
window is armed-until-next-flash, not indefinitely armed. Once the bootloader is `0x2101`, the
ordinary transceiver path above becomes the recovery mechanism for any future re-flash: the
`bootloader:` block goes inert (a same-bootloader chip has nothing left to upgrade) and every press
uses the plain flash sequence.

**What a successful run looks like.** Turn the switch on, press the button once, then leave it
alone. Expect roughly:

```
LR1121 bootloader rewrite: arming switch is on -- running the three-stage sequence now.
LR1121 bootloader rewrite: Stage 1a (loader write): erasing radio flash, this takes a few seconds...
LR1121 bootloader rewrite: Stage 1a (loader write): erase took 2474 ms, write took 584 ms
LR1121 bootloader rewrite: Stage 1b checkpoint passed -- chip in transceiver mode: type=0xDE fw=33.0
LR1121 bootloader rewrite: Stage 2 -- rewriting the bootloader now. This step cannot be undone.
LR1121 bootloader rewrite: Stage 2 chip accepted UpdateBootloader (command_status=2)
LR1121 bootloader rewrite: Stage 2 complete -- bootloader is now 0x2101.
LR1121 bootloader rewrite: Stage 3 (transceiver write): erase took 2474 ms, write took 1945 ms
LR1121 firmware update: success -- now running 1.4
```

The **Stage 1b checkpoint** line is the meaningful one to watch for: everything up to that point is
still fully recoverable, and it is what confirms the loader really is running before the
irreversible step begins. After it, the device goes quiet for a few seconds — that is the flash, not
a crash. Once it reboots, the startup log should report bootloader `0x2101` and firmware `1.4`.

If anything goes wrong the log says which stage failed and whether it is recoverable, in plain
terms, rather than leaving you to work it out. A stage 3 failure in particular is *not* serious: the
bootloader is already updated, and the ordinary flash button (switch off) finishes the job.

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

### Named manufacturers

The `manufacturer` key on a 1W `oneway_controllers:` identity (see "Sending 1W Commands" above)
accepts these named values, the IO-Homecontrol alliance's own manufacturer IDs:

| Name | Hex ID | Name | Hex ID |
|---|---|---|---|
| `velux` | `0x01` | `window_master` | `0x07` |
| `somfy` | `0x02` | `renson` | `0x08` |
| `honeywell` | `0x03` | `ciat` | `0x09` |
| `hormann` | `0x04` | `secuyou` | `0x0A` |
| `assa_abloy` | `0x05` | `overkiz` | `0x0B` |
| `niko` | `0x06` | `atlantic_group` | `0x0C` |

A manufacturer not in this table can still be declared as a raw hex ID, e.g. `manufacturer: 0x0D`.

## Linked Remotes

Physical IO-Homecontrol remotes (wall switches, handheld remotes, wind sensors) use the 1W (one-way) protocol to send commands. Unlike 2W devices that address a specific device ID, 1W remotes broadcast to a type-class address (e.g., "all awning devices"). This means the controller cannot automatically detect which of your devices a particular remote controls — you need to configure the link explicitly.

**`linked_remotes` and `exposed_senders` are receive-only.** They only change how the hub *reacts*
to 1W radio traffic it overhears from other transmitters — decoding intent, firing events,
updating linked devices — and neither one transmits anything. That is a separate capability: the
hub can also transmit 1W commands of its own, including to 1W-only devices with no 2W/authenticated
protocol at all. See "Sending 1W Commands (Controller Identities)" above for the transmit side.

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