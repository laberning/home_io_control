# ADR 0024: Diagnostic probes are gated, paired-devices-only, and isolated from the status decoder

**Status:** Accepted · **Recorded:** 2026-08

## Context

A handful of opcodes seen in field logs have no builder anywhere in this codebase:
`CMD_PRIVATE` (0x03) with a non-default function ID, extended `CMD_PRIVATE` selector 0x80,
`CMD_GET_GENERAL_INFO3` (0x58), and `CMD_PRIVATE2` (0x0C). Nothing in this project can emit any
of them, so the open questions around what they mean (battery/telemetry reads, a possible
stored-position readback) cannot be closed by analysis alone — closing them needs a mechanism to
send these frames to a real device and capture what comes back.

That mechanism is inherently different from the rest of this component's action surface.
Every other action (rename, identify, force-open, scan) sends a *known, decoded* command. A
diagnostic probe sends a command whose reply format is, by definition, not yet understood. Two
risks follow directly from that:

1. **An undecoded reply can be misread by existing code that was never built to receive it.**
   `hub_status.cpp`'s `PRIVATE_RESPONSE_MIN_DATA_LEN = 6` gate exists to accept the ordinary
   position-status `CMD_PRIVATE_RESP`, which is also 6+ bytes. A probe's function-ID reply (e.g.
   `04 60 00 12 00 00`, a battery reading) clears that same length gate. If it were ever routed
   through `apply_private_response_status()`, a battery value would be published to Home
   Assistant as a cover position. ADR 0022 already means a passively-*overheard* `0x04` cannot
   do this (unauthenticated frames are never applied) — but a probe reply is not overheard, it is
   this hub's own authenticated exchange response, which is exactly the path ADR 0022 leaves
   trusted.
2. **Sending an unknown frame is not risk-free on its own.** Two opcodes in the neighborhood of
   what this instrumentation probes are actively dangerous to guess at: `0x4A`'s leading published
   name is "Delete File", and `CMD_WRITE_PRIVATE` (0x20) is a write whose payload semantics are
   unknown. Neither belongs anywhere near exploratory tooling.

`probe_device`/`probe_sweep` are that mechanism: a general-purpose tool for probing, debugging,
and analyzing the parts of the IO-Homecontrol protocol this codebase does not yet understand —
not a one-off for a single investigation. New opcodes that turn up undecoded in a future capture
get a new probe added to the same mechanism, the same way the opcodes above did. This ADR is the
durable record of the decisions made to build that mechanism safely.

## Options considered

- **No gating, always-on builders.** Rejected outright — sending undecoded opcodes to real
  hardware by default, with no user opt-in, has no justification for a feature whose entire
  purpose is exploratory.
- **A per-device entity, matching the rest of the platform surface.** Rejected for the same
  reason `rename_device`/`identify_device`/`scan_paired_devices` are native API actions rather
  than entities (ADR 0006): this is a rarely-used, advanced operation that would clutter the
  entity UI as an always-visible control on every device. A native API action is the right shape
  regardless of how often any one probe gets used.
- **Route probe replies through the existing status pipeline and rely on callers to interpret them
  correctly.** Rejected on inspection: nothing about a generic `CMD_PRIVATE_RESP` reply
  distinguishes "answer to our own position poll" from "answer to a probe with an unrelated
  function ID" once it reaches `hub_status.cpp`'s dispatcher. The two cannot share a code path
  safely; they need to be structurally separate call graphs, not a runtime flag.
- **A runtime arming switch** (arm before each session, auto-off after a timeout/probe count).
  Tried first, then dropped: the switch added a click-through step and a second piece of runtime
  state without a matching security benefit. Enabling the feature at all already requires editing
  YAML and reflashing — a step only someone who already controls the device's configuration can
  take — so a second, in-Home-Assistant confirmation gated nothing a determined YAML edit
  couldn't already bypass. It also meant every session started with "is the switch still armed,
  or did the window time out," friction with no corresponding safety property.
- **Gate behind a single YAML boolean, route probe replies on their own path, exclude `0x4A` and
  `CMD_WRITE_PRIVATE` entirely.** Adopted.

## Decision

### Gating

Diagnostic probes (`probe_device()` / `probe_sweep()`, exposed as native API actions) are gated
behind one config-time boolean: `home_io_control.diagnostic_probes: true` in YAML. Off by default
— a build that doesn't set it never registers a code path that can send an undecoded probe opcode.
There is no runtime switch, no arm/disarm state, and no session timeout or probe-count budget: the
gate is "was this build compiled with `diagnostic_probes: true`," decided once at flash time, not
a toggle a user flips per session. `IOHomeControlComponent::set_diagnostic_probes_enabled()` is
called once from `__init__.py`'s `to_code()` with the YAML value; `diagnostic_probes_enabled()` is
what `probe_device()` checks before building or sending anything.

Every probe additionally refuses while the target device's last known state is "moving"
(`!dev->is_stopped`): an unknown frame arriving mid-transaction is the one avoidable way a
read-shaped probe could still cause harm, independent of the YAML gate above. This is the
device's last *reported* movement state, not a check on the operation queue or on anything in
flight — `is_stopped` is always `true` for a light/switch (this guard never refuses probes to
those device types), and it can be stale if the device was started from a physical remote the
hub never saw.

### Paired devices only

`probe_device()`/`probe_sweep()` resolve their target through the same `resolve_device_()` every
other management action uses, which requires the device to already be registered on this hub.
This is not incidental: it means a probe can only ever reach a device this hub has already
successfully paired with and holds a key for. There is no path from this instrumentation to an
arbitrary or unpaired device.

### Isolation from the status decoder

`ManagementActions` has no access to `IOHomeControlComponent::update_device_status_()` at all —
that method is `protected` on the hub, and `ManagementActions` is not a friend class and holds no
pointer-to-member into it. A probe reply is read directly out of
`ExchangeEngine::send_and_receive()`'s response frame, serialized to hex, and placed into
`ManagementActionResult::response_hex`/`response_cmd` for reporting. There is no code path by
which a probe's reply can reach `apply_private_response_status()` or any other status-merging
code — this is a structural property of the class boundary, not a runtime check that could be
bypassed by a future edit that forgets it.
`HubManagement.ProbeDeviceFunctionIdReplyNeverUpdatesDevicePosition` (`hub_management_test.cpp`)
pins this as a regression: it feeds a 6-byte function-ID reply through `probe_device()` and
asserts the device's position/target/tilt/stopped fields are untouched.

### `0x4A` is excluded by design, not by omission

`CMD_UNKNOWN4A_REQ = 0x4A` exists as a named constant (so a received frame, if one is ever seen,
renders by name instead of as `UNKNOWN_CMD`), and nothing else. No builder exists for it, no probe
name reaches it, and no code path constructs a frame with that command byte. This is deliberate:

- Its leading published interpretation is destructive — headed "Delete File", with "Rename File"
  as the only competing definition, also a write.
- No project this codebase has consulted has ever transmitted it.
- One probe gesture already costs up to `EXCHANGE_RETRY_COUNT` (3) frames on air; guessing at a
  file-management opcode is not a cost worth paying for a question (whether `0x4A` and the
  captured `0x4B` replies are related) that the `0x58`/`0x59`/`0x4B` probe answers indirectly and
  safely: a `0x59` reply to `0x58` implies the captured `0x4B` frames answer something else, and
  `0x4A` is the only remaining candidate in that opcode block.

Grep for `CMD_UNKNOWN4A_REQ` at any time to verify: it should appear only in
`proto_constants.{h,cpp}` (the constant and its `command_name()` case) and in comments explaining
its exclusion — never in a frame builder or a probe table row.

`CMD_WRITE_PRIVATE` (0x20) gets no builder for the same class of reason — it is a write with
unknown payload semantics, and a wrong write to a motor's stored configuration is not recoverable
from the radio side.

### Reporting

Every probe reports the raw reply, not an interpretation — `ManagementActionResult.message` and
the `esphome.home_io_control_action_result` event both carry the response command byte and full
wire hex, deliberately uninterpreted, so the point of the instrumentation (finding out what these
bytes mean) is not pre-empted by a guess baked into the reporting code.

### `probe_sweep()` arguments have no default range

ESPHome native-API action arguments are all required strings — there is no "leave the range alone
and it falls back to a sensible default" path for `first_index`/`last_index`. Every call must pass
an explicit range, which satisfies the original intent (nothing wide happens implicitly) more
strictly than a default would have. Field-observed starting ranges live in
`docs/radio_diagnostics.md`'s per-probe "Start with" column instead.

## Consequences

- Sending an undecoded opcode to real hardware requires an explicit YAML opt-in and a reflash,
  never happens on a default configuration, and is structurally confined to devices this hub has
  already paired with. There is no runtime budget or timeout — once a build is flashed with
  `diagnostic_probes: true`, probes work every time the gate above and the per-device
  not-moving check both pass, for the life of that build.
- A probe reply can never be misread as a position update, tilt update, or stopped-flag change —
  this is guaranteed by class boundaries (`ManagementActions` cannot reach
  `update_device_status_()`), not by a check that a future change could accidentally remove.
- `0x4A` remains permanently unreachable by this codebase unless a future change makes that a
  fresh, explicitly-reasoned decision — this ADR is what that future change would have to
  override, not silently drift past.
- This tool is not scoped to any single investigation. `probe_device`/`probe_sweep` exist to
  answer whatever opcode-level protocol question comes up next, not just the ones that motivated
  building them — a future undecoded opcode gets a new probe added to the same `PROBE_TABLE`
  (`management_actions.cpp`) rather than a new one-off mechanism. An individual probe is a
  candidate for removal only once its opcode's meaning is fully settled: either it graduates into
  a real, decoded feature (e.g. `create_private_function()` becoming the basis of a battery
  sensor), or it is conclusively shown not to carry anything worth decoding on the hardware this
  project has access to. There is no fixed schedule for either outcome.
- Every probe run that produces a reply is expected to become a corpus fixture, understood or not
  — the project's existing growth convention (`tests/corpus/README.md` § "Growth convention"),
  which is what makes each probe's results useful long after the specific question that prompted
  it is answered.
