# Diagnostic probes
<!-- doxygen-label: diagnostic_probes -->

Sending opcodes this project has not fully decoded yet, to learn what a device answers. This is a
deliberately gated feature: probes are read-only reconnaissance, not control.


> [!WARNING]
> **Sends opcodes this project has not decoded — use with caution.** A probe's reply format is,
> by definition, not yet understood. Sending one to a real device is normally low-risk (most of
> these are read-shaped requests real hubs send routinely), but it is not risk-free: an unknown
> command could have effects on the target device that this project cannot predict. Probes only
> ever reach devices already paired to this hub (see below).
> Prefer the field-observed starting values given below over inventing your own, and prefer
> testing on a light/switch over a motor when you do widen — see "Widen carefully" below.

`home_io_control.diagnostic_probes: true` enables two Home Assistant actions, `probe_device` and
`probe_sweep`, for sending a handful of opcodes this project has observed on the wire but never
fully decoded, and for reading back the raw, uninterpreted reply. This is protocol-research
tooling for closing exactly that kind of open question on hardware you own — see
[ADR 0024](adr/0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md) for how it is
gated and isolated from the rest of this component.

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
  the point of a probe is that the reply's meaning is not yet known. `probe_device` puts it in the
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
| `get_info1` | `CMD_GET_INFO1` (0x54), no payload | — (`probe_device` only; `probe_sweep` rejects it) | — | Field-observed: a real hub sends `0x54` on air. No `0x55` answer has ever been captured, so this probe may well draw an `0xFE` "opcode not supported" or nothing at all — that is itself a result worth recording. |
| `get_info2` | `CMD_GET_INFO2` (0x56), no payload | — (`probe_device` only; `probe_sweep` rejects it) | — | The **request** has never been captured; it rests on the protocol's even=request / odd=answer pairing rule. The **answer** `0x57` is captured and carries a leading ASCII reference string followed by the packed type/subtype bytes this component already decodes. Reply strings are wire-observable and citable as-is; do not attempt to resolve one to a model name from any non-public source. |
| `general_info3` | `CMD_GET_GENERAL_INFO3` (0x58), no payload | — (`probe_device` only; `probe_sweep` rejects it) | — | — |
| `private2` | `CMD_PRIVATE2` (0x0C), long wire form | The modifier byte | `0x06`, then `0x05`, `0x09` | Field-observed: a real hub sends exactly these modifier bytes in the long form to real motors (e.g. request data `D4 00 80 D8 06 00`). This component's own `POS_FAVORITE`/`POS_VENT_MODIFIER` values (`0x00`/`0x03`) are not what a hub sends, and never draw the extended block described below. |
| `private2_short` | `CMD_PRIVATE2` (0x0C), short wire form | Same as `private2` | `0x03`, then `0x09` | Field-observed: a real hub sends the short form with these modifiers (e.g. request data `D8 03 00 00`). |

**There is deliberately no probe for `0x4A`.** Everything known about it points at a destructive
file-management operation, and nothing has ever been observed transmitting it on air. See
[ADR 0024](adr/0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md).

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
| `status_ext_fn6`, index = block | `data[2..3]` zeroed, same as `private_fn` fn `0x06`. Walking past the last block the device implements draws `ERROR_RESP` result code `0x58`, mapped as `INVALID_FUNCTION_INDEX` — seen cross-vendor (Somfy Sunea awning + dimmer at block `0x80`; a Velux window at block `0x0F` and every block `0x80`–`0x8F`). Not a documented result code; treat it purely as "that index does not exist on this device". |
| `private_fn_sub` (fn `0x09`), index = `data[1]` | Byte-identical to the `private_fn` fn `0x09` reply on both mains devices — the non-zero second payload byte changed nothing. On a Velux window a non-zero `data[1]` flipped the reply's `data[0]` to `0x2D` and, at an irregular set of sub-indices, appended a `00 20` field — both undecoded. |
| `get_info2` (`0x56`) | `0x57` reply: 10 printable ASCII bytes (`5143802A06`, `5071662B09`, `5165948A01`) — a Somfy-internal reference / sw-version code, **not** a public catalogue number. The next two bytes (`data[10..11]`) are the packed device type/subtype this component already decodes via `decode_packed_device_type()` — verified: dimmer → `LIGHT`, awning → `HORIZONTAL_AWNING`, Velux window → `WINDOW_OPENER`. `data[12..15]` undecoded. |
| `get_info1` (`0x54`) | Not yet sent to a device by this project. No `0x55` answer has ever been captured from anything. |
| `general_info3` (`0x58`) | Dimmer answered a real `0x59`; a Somfy awning and a Velux window both replied `ERROR_RESP` result `0x08` (`ERROR_DURING_EXECUTION`, "opcode not supported"). Device-dependent. |
| `private2` / `private2_short` (`0x0C`), index = modifier | `0x0D` reply; `D4 00` is the request's own leading bytes echoed back; optional `0x80` block is position family. Byte-identical day vs night across 17 solar devices, and byte-identical between two window positions on a Velux window (a stored parameter, not a live reading) — its short form at modifier `0x03` read back the stored ventilation position (`BA 00`). See "Reading a `private2` reply" below. |

**What the replies do and don't carry.** Across every probe and device tried so far — mains and
solar, day and night — the only things recovered are position/target data and, via `status_ext` /
`private_fn` `data[8..10]`, the last commanding node ID. No reply has carried a per-device
sensor value (battery, charge, luminance, temperature). Out-of-range indices answer result code
`0x58` (`INVALID_FUNCTION_INDEX` — self-derived from the cross-vendor pattern, no external
source). The remaining non-position unknowns — `data[0] = 0x2D` framing, the `00 20`
`private_fn_sub` field, `get_info2` `data[12..15]` — have no decode and no external source.

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
