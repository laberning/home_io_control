# ADR 0082: Heater exchanges share the exchange bookkeeping

<!-- doxygen-label: adr0082 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0033](0033-heating-send-path-bypasses-the-cover-machinery.md) (decision 1: the heating
send is a plain exchange with its own diagnostics calls).

## Context

ADR 0033 kept heater and water heater exchanges away from the cover's send-and-settle helper, which
decodes a status reply and schedules poll backoff. To keep the diagnostics alive, the heater path
called the link-health, exchange-failure and result-code helpers itself. Since then the shared
exchange helper that every cover, light and sensor poll uses has gained rules the heater copy does
not have: an exchange that was challenged but never closed counts as an exchange failure, and a
device event report that arrives during the exchange is recorded and the request asked once more.
ADR 0080 also expects the end of every exchange to move the catch-up read out, but only queued
operations reach that hook. A heater exchange today only stamps last contact when it is challenged
and never closed, drops a device event report, and a heater write that a climate or water heater
entity makes during a linked-remote burst does not move the catch-up read.

## Decision

1. **A heater or water heater exchange runs through the shared exchange helper**, the same one the
   status polls use, with the heater's own attempt cap and no poll backoff from the helper (the heater
   poll keeps its own backoff). The heater path keeps only what is heater-specific: the check that
   the answer is a private-register acknowledgement, and its log lines.
2. **The shared rules apply to heaters unchanged.** A challenged exchange that is never closed counts
   one exchange failure. A device event report during a register read or write is recorded and the
   request is sent once more; writing the same register value twice changes nothing on the device.
   An error reply records its result code.
3. **Every exchange the hub runs ends with the end-of-exchange hook**, so a heater or water heater
   exchange moves the catch-up read like any other. The hook after each queued operation stays,
   because it also covers operations that do not use the hub's exchange wrapper.

The rest of ADR 0033 stands: no status decode, no optimistic overlay, one gate for every heating
entry point.

## Consequences

- No frame on air changes, apart from the one repeated request after a device event report.
- The Exchange Failures and Last Device Event sensors of a heater behave as they do for a cover.
- One copy of the exchange bookkeeping: a later rule added to the shared helper reaches heaters
  without a second edit.
