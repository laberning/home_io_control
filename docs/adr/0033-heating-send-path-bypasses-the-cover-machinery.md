# ADR 0033: The heating send path bypasses the cover status/optimistic machinery

**Status:** Accepted · **Recorded:** 2026-09

## Context

The experimental 2W heating/climate support (`CMD_WRITE_PRIVATE` 0x20 — Atlantic
/ Thermor / Sauter "Cozytouch" radiators) adds one hub transmit method,
`IOHomeControlComponent::send_heating_command()`, plus an `IOHomeClimate` entity
and a `heating_control` hub action that both call it. Two existing hub
mechanisms were candidates to reuse and were deliberately not used.

**`execute_request_and_update_()`** is the shared send-and-settle helper for
covers, lights, switches and locks. It decodes the reply through
`update_device_status_()` and schedules poll backoff. Both behaviours are
cover/position-shaped: a heater reports no position, has no status poll, and
feeding a `0x21` write-private ack into the status decoder would be the same
category error ADR 0024 keeps the diagnostic-probe path away from. The heating
protocol also has no readback at all — the `set_*` functions are write-only, and
`power_on` / `midnight_sync` are register reads whose ack payload is logged at
DEBUG but never decoded into a field.

**The ADR 0030 optimistic-state overlay** (`OptimisticState` on `IoDevice`,
`apply_optimistic_*` / `rollback_optimistic()`) exists to hold a hub *prediction*
apart from a competing device *observation*, withdrawing the prediction on
command failure so it can never corrupt a real reading. Its fields are
`target` / `tilt` / `motion` — cover-shaped, with no temperature or HVAC-mode
slot — and, more fundamentally, heating has no observation stream for a
prediction to sit against. There is nothing to overlay, nothing to be
superseded, and nothing a rollback-vs-observation split would mean. Adding
heating fields to `OptimisticState` would put values in it that can never be
superseded by real data, weakening ADR 0030's invariant.

## Decision

1. **`send_heating_command()` talks to the radio directly.** It does a plain
   `send_and_receive_()` on `FREQ_CH2`, treats `response.cmd ==
   CMD_WRITE_PRIVATE_ACK` as success and anything else (including
   `CMD_ERROR_RESP`) as failure. It does **not** route through
   `execute_request_and_update_()`. It still calls the device-agnostic
   `detail::update_link_health()` / `detail::record_exchange_timeout()` /
   `detail::record_command_result()` helpers so a heating exchange still feeds
   the Last Contact / Exchange Failures / Last Result Code companion sensors —
   those are the only feedback a write-only device can give.

2. **The `IOHomeClimate` entity does not use the optimistic overlay.** It
   publishes entity state **only after** a send returns success, never at
   request time, and describes every published value as "last commanded, never
   confirmed" in code, docs and doxygen. It calls none of `apply_optimistic_*`
   or `rollback_optimistic()`. Because the send path is synchronous,
   publish-on-success is sufficient and no rollback hook is needed.

## Consequences

- One transmit path, one caller of `create_write_private()`: the entity and the
  action share `send_heating_command()`, so there is exactly one place heating
  frames are built and sent.
- A heating device is never silently invisible to link-health diagnostics
  despite skipping the cover settle path.
- If heating sends are ever moved onto `OperationQueue` (they are not here),
  this decision must be revisited: a queued dispatch can fail long after
  `control()` returned, which is exactly why the queued cover path needs
  `rollback_optimistic()`.
- Nothing on the wire changes from this ADR: it records why two existing
  helpers are bypassed, not a new frame or decoder.
