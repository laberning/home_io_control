# Diagnostic probes
<!-- doxygen-label: diagnostic_probes -->

Sending opcodes this project has not fully decoded yet, to learn what a device answers. This is a
deliberately gated feature. Every probe but two is read-only reconnaissance; the exceptions,
`subscribe_event` and `unsubscribe_event`, change the device and are marked as such below.
`read_object` reads a device object in several frames and is described in [Reading a device object](#reading-a-device-object).


> [!WARNING]
> **Sends opcodes this project has not decoded — use with caution.** A probe's reply format is,
> by definition, not yet understood. Sending one to a real device is normally low-risk (most of
> these are read-shaped requests real hubs send routinely), but it is not risk-free: an unknown
> command could have effects on the target device that this project cannot predict. Probes only
> ever reach devices already paired to this hub (see below).
> Prefer the field-observed starting values given below over inventing your own, and prefer
> testing on a light/switch over a motor when you do widen — see "Widen carefully" below.

> [!WARNING]
> **`subscribe_event` writes a subscription into the device, and no confirmed way to remove it is
> known.** `unsubscribe_event` is an untested attempt at one. A battery-powered device may spend
> battery on the reports it then sends. Use it on one device and one event at a time, start with
> `index` 1, and read the table rows below before you run it.

`home_io_control.diagnostic_probes: true` enables two Home Assistant actions, `probe_device` and
`probe_sweep`, for sending a handful of opcodes this project has observed on the wire but never
fully decoded, and for reading back the raw, uninterpreted reply. This is protocol-research
tooling for closing exactly that kind of open question on hardware you own — see
[ADR 0024](adr/0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md) for how it is
gated and isolated from the rest of this component, and
[ADR 0060](adr/0060-a-state-changing-diagnostic-probe.md) for the one probe that changes a device.

```yaml
home_io_control:
  node_id: "C0FFEE"
  system_key: "..."
  diagnostic_probes: true
```

**It only ever targets a device already paired to this hub.** `probe_device`/`probe_sweep`
resolve their target the same way every other management action does — there is no path from this
instrumentation to a device this hub has not already paired with and does not already hold a key
for. Every probe additionally refuses while the target device's last known state is "moving" —
an unknown frame is never sent into a device state machine that is already mid-transaction. This
is the device's last reported movement state, not a check on anything in flight: it never applies
to a light/switch, and it can be stale if the device was last moved from a physical remote the hub
never saw.

## Calling the actions

Same node-scoped naming as every other action in this component — see
[Home Assistant actions](configuration/actions.md) for the full explanation of
how `<node_name>` is derived from `esphome.name`. For a config with `name: hioc-heltec-v2`:

```yaml
action: esphome.hioc_heltec_v2_probe_device
data:
  device_id: "FEEB1E"
  probe: "private2"
  index: "0x09"
```

`probe_sweep` takes a range instead of a single `index`:

```yaml
action: esphome.hioc_heltec_v2_probe_sweep
data:
  device_id: "FEEB1E"
  probe: "status_ext"
  first_index: "0x00"
  last_index: "0x01"
```

- `device_id` (required, both actions): the 6-hex-character IO-homecontrol device ID, same as
  every other management action.
- `probe` (required, both actions): which frame to send — see the table below.
- `index` (`probe_device`) / `first_index` + `last_index` (`probe_sweep`): always plain strings —
  `"6"` and `"0x06"` are both accepted; anything else is rejected with a clear error rather than
  silently defaulting to `0`. `probe_sweep` is bounded to 16 indices per call, spaced a second
  apart, and reports one line per index (answered / error-coded / silent / refused).
- Every reply is reported as raw hex plus its command byte, deliberately uninterpreted, because
  the point of a probe is that the reply's meaning is not yet known. The one exception is a
  limitation reply (`0x26`): the message adds a decoded line labelled `assumed layout` beside the
  raw hex. `probe_device` puts it in the
  result event's `response_cmd`/`response_cmd_name`/`response_hex` fields; a sweep's per-index
  replies are inline in `message` — see [Home Assistant actions](configuration/actions.md). Every
  successful reply is also logged
  at the `io_capture` DEBUG tag (the same structured logging every other received frame uses), so
  with `logger: level: DEBUG` a captured reply pastes directly into `scripts/corpus/ingest.py`
  with no reformatting — this does not require the `-DIOHOME_FRAME_LOG` build flag.

## Available probes

| `probe` | Sends | `index` selects | Start with | Evidence for the starting values |
|---|---|---|---|---|
| `private_fn` | `CMD_PRIVATE` (0x03) with a chosen function ID | The function ID | `0x06` or `0x09` | Not field-observed on the wire — every captured `CMD_PRIVATE` frame uses function ID `0x03`. On real hardware (17 solar devices plus this project's own mains motors) `0x06` and `0x09` returned a stored/target **position**: `0x06` is always `00 00`; `0x09` tracked shutters closing. Not a battery probe. |
| `private_fn_sub` | `CMD_PRIVATE` (0x03) at function ID `0x09`, with a chosen second payload byte | The second payload byte (`data[1]`) | `0x01` | **Not field-observed.** Every `CMD_PRIVATE` frame ever captured — by this project or by a real hub — has `0x00` in this byte, so nothing on air pins down what it means. Treat any reply as uninterpreted. |
| `status_ext` | Extended `CMD_PRIVATE` at selector `0x80` | The block/`N` value | `0x00` and `0x01` | Field-observed: real hubs send exactly these two values to real motors. |
| `status_ext_fn6` | Extended `CMD_PRIVATE` at selector `0x80`, function ID `0x06` | The block/`N` value | `0x00`, then `0x80` | The 4-byte extended **shape** is field-observed: a real hub sends `03 80 00 00` and `03 80 01 00` to real motors (40 frames across two logs), and the `0x80` at `data[1]` is what makes the `0x04` reply carry its trailing extended block. The **function ID** `0x06` in that shape is *not* field-observed — no hub has ever been seen sending it. `probe_sweep` caps a run at 16 consecutive indices, so `0x80` is out of reach of a sweep starting at 0 — send it with `probe_device` (or sweep `0x80`–`0x8F` explicitly). |
| `status_ext_fn9` | Extended `CMD_PRIVATE` at selector `0x80`, function ID `0x09` | The block/`N` value | `0x00`, then `0x01` | Same shape evidence as `status_ext_fn6`; the function ID `0x09` in this shape is not field-observed. On the ordinary 3-byte form, `0x09` returned a stored position on every device tested — not a battery value. |
| `get_info1` | `CMD_GET_INFO1` (0x54), no payload | — (`probe_device` only; `probe_sweep` rejects it) | — | Field-observed: a real hub sends `0x54` on air. Two `0x55` answers are in the corpus (`velux_window_probe_metadata_replies`, `somfy_rs100_probe_get_info1_sx1262`); their layout is not decoded. A device may also draw an `0xFE` "opcode not supported" or nothing at all — that is itself a result worth recording. |
| `get_info2` | `CMD_GET_INFO2` (0x56), no payload | — (`probe_device` only; `probe_sweep` rejects it) | — | The **request** has never been captured; it rests on the protocol's even=request / odd=answer pairing rule. The **answer** `0x57` is captured and carries a leading ASCII reference string followed by the packed type/subtype bytes this component already decodes. Reply strings are wire-observable and citable as-is; do not attempt to resolve one to a model name from any non-public source. |
| `general_info3` | `CMD_GET_GENERAL_INFO3` (0x58), no payload | — (`probe_device` only; `probe_sweep` rejects it) | — | — |
| `private2` | `CMD_PRIVATE2` (0x0C), long wire form | The modifier byte | `0x06`, then `0x05`, `0x09` | Field-observed: a real hub sends exactly these modifier bytes in the long form to real motors (e.g. request data `D4 00 80 D8 06 00`). This component's own `POS_FAVORITE`/`POS_VENT_MODIFIER` values (`0x00`/`0x03`) are not what a hub sends, and never draw the extended block described below. |
| `private2_short` | `CMD_PRIVATE2` (0x0C), short wire form | Same as `private2` | `0x03`, then `0x09` | Field-observed: a real hub sends the short form with these modifiers (e.g. request data `D8 03 00 00`). |
| `status_mp_fp` | `CMD_PRIVATE` (0x03) function `0x01`, FPI mask `0xFE` (main parameter plus FP1-FP7), no index | — (`probe_device` only; `probe_sweep` rejects it) | — | The same read-only status request a VELUX KLF 200 gateway sends to its devices (public KLF 200 API specification v3.18, section 10.3.1.4), heard on air at 868 MHz and 2.4 GHz. This is the gateway's position and status poll: it does not read a limitation, so it cannot show rain protection (use `limitation` for that). The reply layout is not decoded: flags `2D`, the main parameter, then the parameter values. |
| `limitation` | `CMD_LIMITATION_STATUS_REQ` (0x25) with data `{selector, 00, 00}` | `0` = resulting minimum, `1` = resulting maximum (any other value is rejected; `probe_sweep` accepts `0` to `1`) | `0`, then `1` | The request pair a VELUX KLF 200 gateway sends to its windows every five minutes, heard on air at 868 MHz in issue #98 (selector `80`, then `C0`). Its purpose follows the KLF 200 API specification v3.18, section 10.5 (get limitation: resulting minimum and maximum). The value layout is confirmed on a VELUX window dry and under rain; the originator and remaining-time bytes are unverified. |
| `subscribe_event` | `CMD_SUBSCRIBE_EVENT` (0x73) with data `01 <event hi> <event lo>` | `0` = battery level event `0x2001`, `1` = local sensor event `0x2005` (any other value is rejected; `probe_sweep` refuses this probe) | `1` | **Index `1` accepted by an RS100 Solar io** ([issue #156](https://github.com/laberning/home_io_control/issues/156)); index `0` is untested. The one probe that changes a device: it asks the device to report the event from now on, and no way to cancel that is known. Only those two events can be sent. A try the device authenticated is not repeated, so a device that takes the subscription and says nothing back ends as "accepted but no response". The RS100 answered at once with its first report, which the probe result names (`[event 0x2005, 5 data bytes]`) and puts on the "Last Device Event" sensor. Reports are recorded, not decoded: see "Last Device Event" below. |
| `unsubscribe_event` | `CMD_SUBSCRIBE_EVENT` (0x73) with data `00 <event hi> <event lo>` | Same as `subscribe_event` (`probe_sweep` refuses it too) | `1` | **Untested guess.** The subscribe request with its first byte cleared, as an attempt to undo a subscription. Whether it works shows only in whether the device's reports stop on "Last Device Event" afterwards; an answer to the request alone proves nothing. Same guards and one-accepted-try rule as `subscribe_event`. |
| `read_object` | `CMD_TRANSFER_OPEN` (0x46), then `CMD_UNKNOWN4A_REQ` (0x4A) blocks, then a close | `0` = actuator capacity record `0x8100`, `1` = actuator state record `0x8103`, `2` = name record `0x8003`, `3` = general info 1 `0x8001`, `4` = general info 2 `0x8002` (any other value is rejected; `probe_sweep` refuses this probe) | `2` first, then `3`, `4`, `0`, `1` | Reads, never writes. See [Reading a device object](#reading-a-device-object). |

A `get_info2` reply (`0x57`) leads with printable ASCII: paste the raw hex into the corpus and
read the string off it, but record only what the wire shows — do not resolve it to a model name
from any non-public source.

## What each probe and index has returned

A running record of what these frames actually draw back — as much as is understood so far.

| probe / index | what came back |
|---|---|
| `private_fn` fn `0x06` | `data[2..3]` = `00 00` on every device probed — two mains motors of ours plus 17 solar shutters via a field reporter. No content. |
| `private_fn` fn `0x09` | `data[2..3]` = a stored position: `D8 0A` on the dimmer (`0xD8` == `POS_FAVORITE`), `58 22` on the awning; solar shutters in the field data tracked their real position as they closed. |
| `status_ext` (fn `0x03`), index = block | Field-observed selector. Device-dependent framing (see `status_ext_fn9`). Blocks `0x00`/`0x01` are what real hubs send. |
| `status_ext_fn9`, index = block | Reply framing is device-dependent: some devices answer `data[0]` = the `0x04`/`0x05` stopped-flag byte with an `0x80`-tagged block, others answer `data[0] = 0x2D` with no block. On the dimmer the `0x80` block **tail changes with the index** (`… 80 00 00 00` at `0x00` → `… 80 D8 06 00` at `0x01`) — first time one of our own probes drew a non-empty, index-selected block; content is position family (`D8 06`). The awning ignores the index. A Velux window kept `data[2..3]` = its live position and appended an index-selected `0x80` tail (`78 00` / `50 00` / `C8 00` at different blocks — position-family values, meaning undecoded). |
| `status_ext_fn6`, index = block | `data[2..3]` zeroed, same as `private_fn` fn `0x06`. Walking past the last block the device implements draws `ERROR_RESP` result code `0x58` (`TOTAL_INCOHERENCE`) — seen cross-vendor (Somfy Sunea awning + dimmer at block `0x80`; a Velux window at block `0x0F` and every block `0x80`–`0x8F`). Read it as "that index does not exist on this device". |
| `private_fn_sub` (fn `0x09`), index = `data[1]` | Byte-identical to the `private_fn` fn `0x09` reply on both mains devices — the non-zero second payload byte changed nothing. On a Velux window a non-zero `data[1]` flipped the reply's `data[0]` to `0x2D` and, at an irregular set of sub-indices, appended a `00 20` field — both undecoded. |
| `get_info2` (`0x56`) | `0x57` reply: 10 printable ASCII bytes (`5143802A06`, `5071662B09`, `5165948A01`) — a Somfy-internal reference / sw-version code, **not** a public catalogue number. The next two bytes (`data[10..11]`) are the packed device type/subtype this component already decodes via `decode_packed_device_type()` — verified: dimmer → `LIGHT`, awning → `HORIZONTAL_AWNING`, Velux window → `WINDOW_OPENER`. `data[12..15]` undecoded. |
| `get_info1` (`0x54`) | A VELUX window answered `0x55` with `00 00 00 00 00 00 00 00 00 30 03 00 00 00` (corpus `velux_window_probe_metadata_replies`). An RS100 Solar io answered with the ASCII bytes `5163341E02` and then `04 00 FF FF` (corpus `somfy_rs100_probe_get_info1_sx1262`). Nothing decodes either yet. |
| `subscribe_event` (`0x73`) | Index `1` on an RS100 Solar io: accepted behind a challenge, answered with a `0x71` carrying event `0x2005` and five data bytes, `06 00 00 00 00`, at night (corpus `somfy_rs100_probe_subscribe_event_sx1262`). The value bytes are not decoded yet. Index `0` has not been sent. |
| `unsubscribe_event` (`0x73`, action `00`) | Not yet sent to a device. Run it only after `subscribe_event` drew reports, then record whether they stop. |
| `read_object` (`0x46`/`0x4A`) | Somfy Izymo dimmer, protocol version 3 (`get_info1` byte 11 = `03`): index `2` opened plainly (`47` accepted, 16 bytes), the block reads were challenged and answered, and the record is `00` + "MODULE DIMMER" + padding. Indexes `0` and `1` were refused with status 3 and length 0. Index `3` returned the 14 bytes of the `get_info1` answer and index `4` the 16 bytes of the `get_info2` answer, byte for byte. |
| `general_info3` (`0x58`) | Dimmer answered a real `0x59`; a Somfy awning and a Velux window both replied `ERROR_RESP` result `0x08` (`COMMAND_UNKNOWN`: the device does not implement the command). Device-dependent. |
| `private2` / `private2_short` (`0x0C`), index = modifier | `0x0D` reply; `D4 00` is the request's own leading bytes echoed back; optional `0x80` block is position family. Byte-identical day vs night across 17 solar devices, and byte-identical between two window positions on a Velux window (a stored parameter, not a live reading) — its short form at modifier `0x03` read back the stored ventilation position (`BA 00`). See "Reading a `private2` reply" below. |
| `status_mp_fp` | Unauthenticated, answered with a `0x04` and no challenge. A reply heard from a window: `04 2D 60 00 00 80 00 00 00`. The main parameter is `00 00` for open and `C8 00` for closed, followed by `80 00 00 00`. Nothing is applied to the device record. |
| `limitation` | Unauthenticated, as heard from a KLF 200. Two replies heard from a window with no limitation active: `26 00 00 00 00 00` after the minimum request and `26 00 C8 00 00 00` after the maximum request. The result message adds a decoded line, `limitation (assumed layout): param=MP value=100% (C8 00) originator=local_user(0x00) time=0x00 (30 s)`, which reads the five data bytes as parameter, value, originator and remaining time. Under rain protection the same window answered the minimum request with `26 00 BA 00 00 00` (93 %) and the maximum request unchanged. The value bytes read as described; the originator and remaining-time bytes were `00` even then, so those two stay an assumption, and the raw bytes stay beside every decoded value. A Somfy awning answers behind an authentication challenge with `26 05 00 00 00 00` (minimum) and `26 05 C8 00 00 00` (maximum) at every awning position: the first byte is `05`, so the decode shows a non-main parameter. Nothing is applied to the device record. |

**What the replies do and don't carry.** Across every probe and device tried so far — mains and
solar, day and night — the only things recovered are position/target data and, via `status_ext` /
`private_fn` `data[8..10]`, the last commanding node ID. No reply has carried a per-device
sensor value (battery, charge, luminance, temperature). Out-of-range indices answer result code
`0x58` (`TOTAL_INCOHERENCE`). The remaining non-position unknowns — `data[0] = 0x2D` framing, the `00 20`
`private_fn_sub` field, `get_info2` `data[12..15]` — have no decode and no external source.

## Reading a device object

`read_object` asks a device for one of its stored objects and prints the bytes. Somfy controllers
read the two records at index `0` and `1` from every new actuator right after pairing; they list the
events the actuator can report and the sensor types it carries. Nobody has published their byte
layout, so this probe exists to capture it.

One call does three things: it opens the object, pulls it in blocks of up to 18 bytes, and closes the
transfer. Index `2` is a 16-byte name record; indexes `3` and `4` read the object form of the two info answers (`get_info1`, `get_info2`); on a Somfy Izymo they return the same bytes. Use index `2` first to check that the whole flow works on your device
before you read the records.

```yaml
action: esphome.<your_node>_probe_device
data:
  device_id: "ABC123"
  probe: read_object
  index: "2"
```

The result carries the object id, the transfer's length and the bytes in `response_hex`; the log prints
them 24 bytes to a line. The probe stops without reading when the device answers with an error, refuses
the open, reports a length of 0, or announces more than 288 bytes. It never writes to the device and
never changes the device's state in Home Assistant.

Expect devices to answer in different ways. An error answer or no reply at all means the device does
not offer that object. Post the log lines, including a refusal, when you report a result.

## Last Device Event

With `diagnostic_probes: true` the hub adds a diagnostic text sensor, **Last Device Event**. It shows
the most recent event report from a paired device, or frame from one that nothing else in this
component handles, in the form

```text
#3 t=5421 24635F 0x71 05 60 20 05 05 06 00 00 00 00
```

That is a running counter, the hub's uptime in seconds, the device, the command byte and the payload
in hex. The counter makes two identical reports two different states, which Home Assistant needs to
keep both in its history. The log carries the same frame at INFO level as `Device event from ...`;
a build without `diagnostic_probes: true` logs it at DEBUG only.

- A device event report is recorded whatever its destination. A report is a `0x71` whose event is
  not the position event `0x100A`, or a `0x70`. A `0x71` payload is a flags byte, a status byte, a
  two-byte event id, the length of the event data and the event data. The RS100 Solar io answered a
  subscription to the local sensor event with event `0x2005`; ordinary position updates carry `0x100A`.
- With probes on, any other frame from a paired device that nothing handles is recorded too, so a
  report with an unexpected command byte is not lost.
- A recorded frame never changes a device's position, state or battery; the entity only shows it
  (see [ADR 0060](adr/0060-a-state-changing-diagnostic-probe.md)). A `0x71` addressed to the hub is
  authenticated and acknowledged like every status update before it is recorded; any other recorded
  frame is never answered.
- A report that arrives while the hub waits for the reply to its own request is recorded, and the
  hub asks once more for the reply it was waiting for.

Paste the `Device event` log lines and the sensor's history into an issue; they are what lets the
reports be decoded.

## Reading a `private2` / `private2_short` reply

The reply command byte is `CMD_PRIVATE2_RESP` (**0x0D**), not `0x04`. In every reply captured so
far the two bytes right after the flags byte are `D4 00` — this is the **request's own leading
payload bytes echoed back**, not a `POS_UNKNOWN` position reading. The per-device content, when
there is any, rides in an optional `0x80`-tagged block after that echo:

- Our `0x00` / `0x03` modifiers have only ever drawn the bare echo (`… D4 00 00 00`).
- A real hub's `0x06` modifier draws the block: e.g. `1F3807` answers with `05 D4 00 80 C8 00 00`
  where this component's probe at `0x00` gets `6D D4 00 00 00`.

The block content observed to date is position-family (`C8 00 00`, `72 49 00`) and matches the
`status_ext` last-command tail — it is **not** battery/charge telemetry, and it does not vary over
a day/night cycle (checked on 17 solar devices). `0x0C` is not a live-telemetry read.

## Widen carefully

The starting values above are the safest available for each probe — for `status_ext` and
`private2`/`private2_short` because a real hub sends exactly those bytes to real motors routinely;
for `private_fn` because those two function IDs have drawn harmless position replies on every
device tried. Widening beyond them is a separate, deliberate
decision, not something to do by default — and when you do, prefer the dimmer/light over a motor: a
wrong write-shaped result on a light is visible and trivially reversible, while a motor's stored
configuration is not.

`private_fn_sub`, `status_ext_fn6` and `status_ext_fn9` differ from `status_ext`/`private_fn` in
that only their *shape*, not their *function ID* or *sub-index*, is field-observed — so they are one
byte further from known-safe traffic than anything else in this table. The same "prefer the
dimmer/light over a motor" advice applies, with more reason.

## Expect a long block from `probe_sweep`

A full-range sweep can block the ESPHome loop (API, other components, OTA) for on the order of a
minute at default tuning, longer if a device never answers or if `exchange_start_response_wait_ms`
has been raised — up to 16 indices, each up to 3 retries at the configured response-wait time,
plus a spacing delay between indices. This is
accepted deliberately for this maintainer-triggered, explicitly-opted-in diagnostic rather than
restructured into scheduled steps — expect a warning about a long-blocking operation, and expect
other Home Assistant traffic against this device to stall for the duration. `probe_device` (a
single index) does not have this problem; reach for `probe_sweep` only when you actually need the
range in one gesture.

## See also

- [Radio tuning](configuration/tuning.md) — the knobs to try before reaching for a probe
- [Diagnostic entities](diagnostic-entities.md) — what the hub reports about a device without probing it
- [Home Assistant actions](configuration/actions.md) — the other hub-level actions
- [Supported devices](supported-devices.md) — where a probe result turns into a matrix row
