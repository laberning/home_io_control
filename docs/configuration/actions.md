# Home Assistant actions
<!-- doxygen-label: cfg_actions -->

Beyond the entities generated from your `cover:`/`light:`/`lock:`/`switch:` YAML, Home IO Control
exposes eleven **hub-level actions** through ESPHome's native API. These are one-off and advanced
operations that would clutter the entity UI if they were always-visible buttons, so most are only
reachable from Developer Tools or an automation — `scan_paired_devices` is the exception, with an
optional one-tap button.

| Action | What it does | `verified` can be `true`? |
|---|---|---|
| [`rename_device`](#rename_device) | Renames a paired actuator and reads the name back to confirm the write. | Yes |
| [`identify_device`](#identify_device) | Makes a device physically identify itself (jog/flash) for its own default time so you can tell which physical motor a device ID belongs to. | No — no readback exists for a jog |
| [`identify_device_for`](#identify_device_for) ⚠️ *experimental* | Identifies for a number of seconds you choose, or stops a running identify with `0` (the stop is confirmed on a VELUX roof-window actuator). | No — no readback exists for a jog |
| [`force_open_device`](#force_open_device) ⚠️ *experimental* | Requests a fully-open move at elevated protocol priority, intended to override wind/rain soft locks. | No — the outcome is asynchronous |
| [`set_position_automatic`](#set_position_automatic) | Moves a cover at automation priority, which a motor locked on manual (a wired A/M switch on M) refuses. | No — the outcome is asynchronous |
| [`scan_paired_devices`](../pairing.md) | Broadcasts a roll-call and reports every already-paired device that answers — no target `device_id`, no arguments at all. Also reachable as a one-tap button via `scan_paired_devices_button: true`. | No — nothing here is read back either |
| [`oneway_set_position`](#oneway_set_position) | Sends a numeric position as a configured 1W controller identity. Takes `controller_id` and `position`, not a `device_id` — 1W addresses a device class. | No — 1W has no reply at all |
| [`oneway_set_sequence`](#oneway_set_sequence) | Sets the rolling number a 1W controller identity's next command carries, so a device that stopped reacting works again without a reflash. Takes `controller_id`, `sequence` and `allow_lower`. Sends nothing. | No — 1W has no reply at all |
| [`oneway_remove_controller`](#oneway_remove_controller) | Removes a configured 1W controller identity from the receivers that are in their programming mode. Takes `controller_id`. | No — 1W has no reply at all |
| [`heating_control`](climate.md) ⚠️ *experimental* | Sends one heating/climate function (`CMD_WRITE_PRIVATE` 0x20) to a climate device. Takes `device_id`, `function`, `value`. | No — an acknowledged write is not a readback; `read_window_open` reports the sensor in the `message`, the `read_paired_devices` / `read_setpoint_block` reads are logged |
| [`pair_device`](#pair_device) | Pairs one device your YAML already declares, addressed by its ID, with no discovery broadcast. Takes `device_id`. | No — the `0x33` confirm is the device's own, and nothing reads a key back out of a device |
| [`pull_key`](#pull_key) | Asks a controller you name by its node ID for its system key and proves it, with no discovery broadcast. Takes `controller_id`. | Yes, for a key the controller or the devices under it proved |

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

Every action except `scan_paired_devices` and the two `oneway_` actions takes `device_id`: the 6-hex-character IO-Homecontrol device ID — the same value you set as `io_device_id` in the entity's YAML, and the same ID the pairing log prints. This is the protocol-level actuator ID, **not** the Home Assistant entity ID. `scan_paired_devices` takes no `data:` at all — it isn't aimed at one device. The two `oneway_` actions take `controller_id`, the `id` of a 1W controller identity, because 1W addresses a device class and no single device.

## Result events

Every action fires the same Home Assistant event, `esphome.home_io_control_action_result`, so one automation trigger can react to any of them:

| Field | When present | Meaning |
|---|---|---|
| `action` | always | Action name, e.g. `rename_device`. |
| `device_id` | always | Target device ID — **empty** for `scan_paired_devices`, which has no single target. For the two `oneway_` actions it holds the `controller_id` you sent. |
| `success` | always | Whether the action succeeded. For `scan_paired_devices`, `true` whenever the broadcast went out — a scan that heard nothing back is a successful scan, not a failure. |
| `verified` | always | Whether a follow-up readback confirmed the result — see the table above for which actions can ever set this `true`. |
| `message` | always | Human-readable outcome summary. For `scan_paired_devices` this is the full multi-line report, not a one-line summary. |
| `requested_name`, `applied_name` | `rename_device` only | Requested vs. verified device name. |
| `result_code`, `result_code_name` | `rename_device`, `identify_device`, `identify_device_for`, `probe_device`, and `heating_control`, when the device replies `CMD_ERROR_RESP` | Decoded protocol result code. |
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

Sends the authenticated `CMD_IDENTIFY` command and asks the device to identify for the time its maker chose, so the jog or flash ends on its own. No device-type gating beyond "is it registered" — identify exists specifically to help you work out what an unknown or unrecognized device physically is. A `CMD_ERROR_RESP` reply still counts as **success**: some devices answer that way to an identify request and jog anyway (confirmed on real hardware — the awning jogged every time despite the error reply).

> [!NOTE]
> A VELUX roof-window actuator (type 4, subtype 1) confirms the device's own time: it jogs, returns to its start position by itself and reports done about 26 seconds after the request. The reporter ran this once, with their own controller rather than this component ([issue #241](https://github.com/laberning/home_io_control/issues/241)). Other devices, Somfy ones included, are not yet confirmed. If a device keeps going, use `identify_device_for` with `seconds: 0` to stop it, and report the device in an issue.

```yaml
action: esphome.hioc_heltec_v2_identify_device
data:
  device_id: "FEEB1E"
```

## `identify_device_for`

Fields: `device_id` (required), `seconds` (required).

> [!NOTE]
> `seconds: 0` is confirmed on a VELUX roof-window actuator (type 4, subtype 1): a window that was still jogging
> finished its return movement and reported done about 5 seconds after the stop. The reporter ran this once, with
> their own controller rather than this component ([issue #241](https://github.com/laberning/home_io_control/issues/241)).
> The stop on other devices, Somfy ones included, and explicit durations of 1 to 253 seconds are experimental; the
> durations follow the public VELUX KLF 200 interface description. A report of what your device does is worth an issue.

Identifies like `identify_device`, but for `seconds` seconds: a whole number from 1 to 253. With `seconds: 0` it stops an identify that is still running. Home Assistant requires every field, so `seconds` is not optional; use `identify_device` for the device's own default time. Any other value (a negative number, a decimal, text, 254 or more) is refused before anything is sent, and the result message names the range.

```yaml
action: esphome.hioc_heltec_v2_identify_device_for
data:
  device_id: "FEEB1E"
  seconds: "5"
```

Stop a running identify:

```yaml
action: esphome.hioc_heltec_v2_identify_device_for
data:
  device_id: "FEEB1E"
  seconds: "0"
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

## `set_position_automatic`

Fields: `device_id` (required), `position` (required, a whole number from `0` = closed to `100` = open, as
`cover.set_cover_position` counts it).

The move an automation sends. It goes out at automation priority (ACEI level 7, byte `0xE7`, as a TaHoma sends its
own commands); the cover entity's moves keep the normal level 3. A Somfy motor following an A/M switch (a wired Smoove Uno io or a Smoove 1 A/M io)
refuses a level-7 command with `PRIORITY_LOCKED` (`FE 38`) while the switch is on M and takes it on A, so a sun or
time automation that uses this action leaves a cover alone while someone has it on manual, and a move from the
dashboard still goes through, like a manufacturer app's "send anyway".

A refusal shows on the cover's Active Issue sensor as `PRIORITY_LOCKED` and turns its
[Manual Mode sensor](cover.md#manual-mode) on; a taken command turns it off. A cover with no lock takes the command
like any other move. As with every cover command, the result event only confirms the command was **queued**.

```yaml
action: esphome.hioc_heltec_v2_set_position_automatic
data:
  device_id: "7A38D1"
  position: "0"
```

## `oneway_set_position`

Fields: `controller_id` (required), `position` (required — a whole number from `0`, fully open, to `100`, fully closed, passed as a string).

Sends the position as the 1W controller identity `controller_id`, with the same four-copy burst as a generated button ([Sending 1W commands](oneway-transmit.md#numeric-positions)). An identity of an inverted device kind mirrors the value on the wire, and the "Last 1W Command" sensor still names what you asked for. The devices the identity lists under [`drives:`](oneway-transmit.md#following-a-device-the-identity-drives) show the position at once and are read afterwards. A `controller_id` that no identity carries, or a `position` that is not a whole number from 0 to 100, is refused before anything is sent, and the result message says which.

```yaml
action: esphome.hioc_heltec_v2_oneway_set_position
data:
  controller_id: velux_windows
  position: "40"
```

The result event reports only that the command was **queued** and never `verified`, because nothing downstream can show that a device moved:

```yaml
action: oneway_set_position
device_id: velux_windows
success: true
verified: false
message: 1W position command queued
```

## `oneway_set_sequence`

Fields: `controller_id`, `sequence` (a whole number from `0` to `65535`, passed as a string) and `allow_lower` (`"true"` or `"false"`; an empty value counts as `"false"`). Home Assistant requires every field, so pass `allow_lower: "false"` for an ordinary step forward.

Sets the number the next command of the 1W controller identity `controller_id` carries, and stores it in flash before the action reports success, so it survives a reboot. Use it when a device ignores every command from the identity because the identity's counter fell behind the number the device remembers, for example after a board replacement or a flash erase. Read the number the hub sent last in the "Last 1W Command" sensor and set a higher one. After a board replacement the sensor shows the new board's numbers: take the last number the old board sent from the sensor's history in Home Assistant, or pick a generously higher number when that history is gone. A Somfy Izymo obeys any higher number ([ADR 0120](../adr/0120-a-one-way-counter-that-wraps-is-lost-to-a-somfy-receiver.md)), so one larger step is enough there.

The action sends nothing. Press the identity's own button afterwards: that command is the test.

A `sequence` that is not above the identity's current number is refused, and the result message names the current number. A device that is in step ignores a lower number, so moving backwards would stop it from reacting until the counter passes its old number again. Set `allow_lower: "true"` only when you mean it, for example to undo a step that went too far. A lower number lasts across a reboot only while it is not below the identity's `initial_sequence:`: otherwise the next boot jumps forward to `initial_sequence:` again ([ADR 0114](../adr/0114-a-one-way-counter-resumes-from-its-persisted-value-across-the-wrap.md)), and the result message says so. Lower `initial_sequence:` in the YAML as well in that case. A `controller_id` that no identity carries, a `sequence` that is not a whole number from 0 to 65535 and an `allow_lower` other than `"true"` or `"false"` (in any letter case) are refused with a reason.

```yaml
action: esphome.hioc_heltec_v2_oneway_set_sequence
data:
  controller_id: velux_windows
  sequence: "4000"
  allow_lower: "false"
```

The result event names the old and the new number and how many commands are left before the counter wraps. A wrapped identity is lost on a Somfy receiver, which ignores every step across 65535 to 0, so a low count is the sign to give the identity a new address and enroll it again ([1W commands do nothing](../troubleshooting.md#1w-commands-do-nothing), step 4):

```yaml
action: oneway_set_sequence
device_id: velux_windows
success: true
verified: false
message: next sequence changed from 1237 to 4000; 61536 commands left before the counter wraps (a wrapped identity is lost on a Somfy receiver)
```

## `oneway_remove_controller`

Fields: `controller_id` (required).

Sends one `0x39` remove-controller burst as the 1W controller identity `controller_id`, nothing else. It carries only that identity's own address, so it cannot remove another controller's registration. A receiver acts on it only while its own programming mode is open; see [Un-enrolling without re-enrolling](oneway-transmit.md#un-enrolling-without-re-enrolling) for the steps.

```yaml
action: esphome.hioc_heltec_v2_oneway_remove_controller
data:
  controller_id: velux_windows
```

The result event reports that the burst was **queued**, never that a device forgot the identity:

```yaml
action: oneway_remove_controller
device_id: velux_windows
success: true
verified: false
message: 1W remove-controller (0x39) queued
```

## `pair_device`

Fields: `device_id` (required).

Pairs one device your YAML already declares, addressed by its ID, with the key exchange alone and
no discovery.

This is the route for a device **another two-way hub still holds**. Discover & Pair cannot reach
one: discovery is the only broadcast in the flow, and a device that holds a controller's key
answers nothing (see
[The device is never found](../troubleshooting.md#the-device-is-never-found)). A declared entity
already carries the address, kind and power class a discovery answer would have reported. The key
pushed is the one the hub authenticates that device with, so the entity's own
[`system_key:`](index.md) when it declares one and the hub's otherwise. The route and its limits
are recorded in [ADR 0129](../adr/0129-a-declared-device-can-be-paired-by-address-without-discovery.md).

> [!WARNING]
> **The hub that held the device loses it.** The device now holds this hub's key, and nothing in
> this route can give it back. Do not use the action on a device you still need on that hub.

The device's pairing window still has to be open, by the gesture its own manual describes, the
same timing Discover & Pair needs. Outside it the device ignores the key transfer and the action
reports no confirm. Nothing here can open that window.

A device no entity declares is refused before anything is sent, which also keeps the action from
being aimed at an address that is not yours. So is a device an earlier pairing registered: add its
YAML entry first. The action always pushes, so it also refuses when the key transfer direction is
pull, which needs the discovery answer this skips: pair such a device with Discover & Pair, which
reaches it anyway, since a device that hands out its own key is one that answers discovery.

```yaml
action: esphome.hioc_heltec_v2_pair_device
data:
  device_id: "FEEB1E"
```

Success means the device sent its `0x33` key confirm. Nothing reads a key back out of a device, so
the result is never `verified`. The "Last Pairing Result" sensor reports the attempt as it does for
Discover & Pair. A request refused before anything is sent is no attempt, so the sensor keeps the
previous result.

## `pull_key`

Fields: `controller_id` (required).

Asks the controller with that node ID for the system key it holds and checks that it holds it. This
is the part of [Discover & Pair](../pairing.md) that follows discovery, aimed at a node you name,
so no discovery broadcast goes out. It is the route for a controller that shares its key only when
you start a mode on it and does not answer a discovery in that mode. The route and its limits are
recorded in [ADR 0130](../adr/0130-a-controllers-key-can-be-pulled-by-its-node-id-without-discovery.md).

To read the controller's node ID, press a button on it. The device log names every controller the
hub overhears, each one at most once in ten minutes:

```text
Heard a request from 4A2F19: ... to ...
```

Start the controller's key-sharing mode, then run the action within that mode's window.

```yaml
action: esphome.hioc_heltec_v2_pull_key
data:
  controller_id: "4A2F19"
```

The result is one of:

- **Proven.** The controller sent a key that differs from the hub's and proved it holds it. The key
  is printed once in the device log with its check value, and the result and the Home Assistant
  event carry the check value and the advice, never the key. Nothing is registered and nothing more
  is sent. Put the key into the `system_key:` of each device that controller operates, kept in
  `secrets.yaml`.
- **Already this hub's key.** The controller already shares the key of this hub. The result is a
  success, not `verified`.
- **Refused.** The controller answered with an error, and the result carries its code. Most often
  the controller is not in its key-sharing mode.
- **Not proven.** The controller sent a key but did not prove it holds it. The hub then does what
  Discover & Pair does: a roll-call under that key. Devices that answer prove the key, the report
  lists them, and they are registered until the next reboot. If none answers, the key is not
  printed and must not be used.
- **No key.** The controller sent nothing. Check the node ID and the key-sharing mode.

The action refuses before anything is sent when the hub is not initialised, when `controller_id` is
not six hexadecimal digits and when it is this hub's own ID. A refused action is no attempt: "Last
Pairing Result" keeps the previous result. Any other outcome is reported there as for Discover &
Pair, with advice codes of its own: `pull_key_proven`, `pull_key_own_key`, `pull_key_refused`,
`pull_key_unanswered`
([advisor codes](../pairing.md#advisor-codes)).

> [!NOTE]
> The route is confirmed on hardware only for nodes that hand over their key when asked, such as an
> Atlantic AGILIA IO radiator in its pairing mode. A remote in its key-sharing mode, a 2.4 GHz
> VELUX remote's "Copy system" for one, is untested.

See [Key extraction](../key-extraction.md#pulling-the-key-from-a-controller-you-name) for how this
fits the other routes to a key.

## See also

- [Pairing](../pairing.md) — `scan_paired_devices`, `pair_device` and the Discover & Pair button
- [Heating and climate](climate.md) — `heating_control`
- [Sending 1W commands](oneway-transmit.md) — the 1W identities that `oneway_set_position`, `oneway_set_sequence` and `oneway_remove_controller` use
