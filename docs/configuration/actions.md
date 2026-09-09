# Home Assistant actions
<!-- doxygen-label: cfg_actions -->

Beyond the entities generated from your `cover:`/`light:`/`lock:`/`switch:` YAML, Home IO Control
exposes six **hub-level actions** through ESPHome's native API. These are one-off and advanced
operations that would clutter the entity UI if they were always-visible buttons, so most are only
reachable from Developer Tools or an automation — `scan_paired_devices` is the exception, with an
optional one-tap button.

| Action | What it does | `verified` can be `true`? |
|---|---|---|
| [`rename_device`](#rename_device) | Renames a paired actuator and reads the name back to confirm the write. | Yes |
| [`identify_device`](#identify_device) | Makes a device physically identify itself (brief jog/flash) so you can tell which physical motor a device ID belongs to. | No — no readback exists for a jog |
| [`force_open_device`](#force_open_device) ⚠️ *experimental* | Requests a fully-open move at elevated protocol priority, intended to override wind/rain soft locks. | No — the outcome is asynchronous |
| [`scan_paired_devices`](../pairing.md) | Broadcasts a roll-call and reports every already-paired device that answers — no target `device_id`, no arguments at all. Also reachable as a one-tap button via `scan_paired_devices_button: true`. | No — nothing here is read back either |
| [`oneway_set_position`](oneway-transmit.md) | Sends a numeric position as a configured 1W controller identity. Takes `controller_id` and `position`, not a `device_id` — 1W addresses a device class. | No — 1W has no reply at all |
| [`heating_control`](climate.md) ⚠️ *experimental* | Sends one heating/climate function (`CMD_WRITE_PRIVATE` 0x20) to a climate device. Takes `device_id`, `function`, `value`. | No — `set_*` are write-only; the `power_on` / `midnight_sync` reads are logged, not decoded |

Two more actions, `probe_device` and `probe_sweep`, exist behind a separate opt-in
(`diagnostic_probes: true`) for sending opcodes this project hasn't fully decoded yet — see
[Diagnostic probes](../diagnostic-probes.md).

## Enabling and triggering actions

Requires a normal `api:` block — Home IO Control enables the extra native-API feature flags this needs internally, so no `custom_services:` or `homeassistant_services:` is needed:

```yaml
api:
  encryption:
    key: !secret api_key
```

Each action becomes a node-scoped ESPHome action named `esphome.<node_name>_<action_name>`. `<node_name>` comes from `esphome.name` (not `friendly_name`), which Home Assistant normalizes to snake_case — e.g. the sample V2 config uses `name: hioc-heltec-v2`, so `rename_device` becomes `esphome.hioc_heltec_v2_rename_device`.

**From Home Assistant Developer Tools → Actions**, use the direct action block (no `alias:`/`sequence:`):

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

Every action except `scan_paired_devices` takes `device_id`: the 6-hex-character IO-Homecontrol device ID — the same value you set as `io_device_id` in the entity's YAML, and the same ID the pairing log prints. This is the protocol-level actuator ID, **not** the Home Assistant entity ID. `scan_paired_devices` takes no `data:` at all — it isn't aimed at one device.

## Result events

Every action fires the same Home Assistant event, `esphome.home_io_control_action_result`, so one automation trigger can react to any of them:

| Field | When present | Meaning |
|---|---|---|
| `action` | always | Action name, e.g. `rename_device`. |
| `device_id` | always | Target device ID — **empty** for `scan_paired_devices`, which has no single target. |
| `success` | always | Whether the action succeeded. For `scan_paired_devices`, `true` whenever the broadcast went out — a scan that heard nothing back is a successful scan, not a failure. |
| `verified` | always | Whether a follow-up readback confirmed the result — see the table above for which actions can ever set this `true`. |
| `message` | always | Human-readable outcome summary. For `scan_paired_devices` this is the full multi-line report, not a one-line summary. |
| `requested_name`, `applied_name` | `rename_device` only | Requested vs. verified device name. |
| `result_code`, `result_code_name` | `rename_device`, `identify_device`, `probe_device`, and `heating_control`, when the device replies `CMD_ERROR_RESP` | Decoded protocol result code. |
| `probe`, `index` | `probe_device` and `probe_sweep` only | Probe name and the requested index (or swept range). |
| `response_cmd`, `response_cmd_name`, `response_hex` | `probe_device` only — a sweep's per-index replies are in `message` | The reply's command byte, decoded command name, and full raw wire hex. |

## `rename_device`

Fields: `device_id` (required), `new_name` (required — UTF-8, ASCII whitespace trimmed, must fit the protocol's 15-character Latin-1 write limit).

Sends the authenticated `SET_NAME` write, then immediately sends `GET_NAME` to read it back — `verified` is `true` only if the readback matches the requested name exactly. An explicit device refusal (`CMD_ERROR_RESP`) surfaces its decoded result code in both the logs and the event.

```yaml
action: esphome.hioc_heltec_v2_rename_device
data:
  device_id: "FEEB1E"
  new_name: "Patio Awning"
```

## `identify_device`

Fields: `device_id` (required).

Sends the authenticated `CMD_IDENTIFY` command. No device-type gating beyond "is it registered" — identify exists specifically to help you work out what an unknown or unrecognized device physically is. A `CMD_ERROR_RESP` reply still counts as **success**: some devices answer that way to an identify request and jog anyway (confirmed on real hardware — the awning jogged every time despite the error reply).

```yaml
action: esphome.hioc_heltec_v2_identify_device
data:
  device_id: "FEEB1E"
```

## `force_open_device`

Fields: `device_id` (required).

> [!NOTE]
> This action is experimental. It is confirmed to move the device correctly, but whether it
> actually overrides an active wind or rain lock is not confirmed. If you can test that safely,
> the result is worth an issue.

Queued through the same dispatch path as the cover entity and its buttons (capability gating, poll tracking, settle handling, backoff) rather than sent directly, so a non-cover device is rejected the same way any other cover command would be. The result event only confirms the command was **queued** — the actual movement shows up later through the device's normal cover-state/polling pipeline, the same as any other cover command.

```yaml
action: esphome.hioc_heltec_v2_force_open_device
data:
  device_id: "FEEB1E"
```

## See also

- [Pairing](../pairing.md) — `scan_paired_devices` and the Discover & Pair button
- [Heating and climate](climate.md) — `heating_control`
- [Sending 1W commands](oneway-transmit.md) — `oneway_set_position`
