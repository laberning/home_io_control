# ADR 0060: A state-changing diagnostic probe

<!-- doxygen-label: adr0060 -->

**Status:** Accepted · **Recorded:** 2026-10

Amends [ADR 0024](0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md): every probe
it describes is read-shaped, and this one is not.

## Context

Some sensors and battery levels reach a hub only if the hub first asks the device to report them. A
device that has been told to report an event sends a frame whenever the event happens; one that has
not been told stays silent, and a status poll never shows the value. To learn what such a device
sends, the hub has to subscribe it, and a subscription is a stored change in the device. That is the
first thing a probe does that cannot be called read-only.

Two facts shape the design. No way to remove a subscription is known, so a mistake is permanent
until the device is reset. And what a subscribed device sends, and to whom, is not known either, so
the reports cannot be decoded yet; they can only be recorded.

## Options considered

- **A permanent action outside the probe mechanism.** Rejected: nothing about the request or its
  reports is decoded, which is exactly the situation ADR 0024's gate exists for.
- **A sweepable probe over every event id.** Rejected: sweeping a request that writes state into a
  device multiplies the permanent change by the range.
- **Passing the event id as the probe's `index`.** Rejected: a typed `0x2001` or an arbitrary 16-bit
  value would reach the builder. A two-value selector, as the `limitation` probe uses, keeps the
  index from being an on-air value.
- **Answering a report the way a status update is answered.** Rejected: nothing says a report wants
  an answer, and an unauthenticated frame must not draw a transmission (ADR 0022).

## Decision

1. **`subscribe_event` is a probe**, behind the same gate as the others: `diagnostic_probes: true`,
   a device already paired to this hub, and a refusal while the device is moving.
2. **Two events, nothing else.** `index` 0 sends the battery-level event `0x2001`, `index` 1 the
   local-sensor event `0x2005`. The builder, `create_subscribe_event()`, checks the id against that
   allowlist itself and returns false for any other, independent of the probe table.
3. **Not sweepable.** The probe table row carries a `sweepable` flag; `probe_sweep` refuses a row
   where it is false, before any index handling.
4. **One accepted try.** A try the device authenticated is never repeated, so a subscription is
   sent once to a device that took it. An unanswered request keeps the ordinary retry budget. The
   rule is `decisions::retry_after_unconfirmed_accept_is_safe()`, the same function that holds
   ADR 0043's re-send rule.
5. **Reports are recorded, never applied.** A `0x70` from a registered device, to any destination,
   is logged, counted and published on the hub's `Last Device Event` text sensor. With probes on,
   any other frame from a registered device that nothing else handles is recorded the same way,
   because what a subscribed device reports with is not known. A recorded frame is unauthenticated
   raw data (ADR 0022): it is never answered, never transmitted to and never merged into device
   state. The same holds when one arrives while the hub waits for the reply to its own request: it
   is recorded, never taken as the reply, and the request is sent once more, since the device is
   awake and its answer usually follows. A second report in a row ends the request as unanswered.
   An `EXECUTE` that is not safe to repeat is not sent again.
6. **The sensor exists only with `diagnostic_probes: true`**, and shows `#<n> t=<uptime s> <SRC>
   <cmd> <payload hex>`. The counter makes a repeated identical report a new state, which Home
   Assistant's history needs.
7. **Logging follows the build.** INFO with probes on; DEBUG otherwise, because a subscription
   outlives the build that made it and must not fill an ordinary build's log.
8. **An untested unsubscribe.** `unsubscribe_event` sends the same request with its first byte
   `00` instead of `01`, a guess from the frame's shape at a way to undo a subscription. It has the
   same allowlist, guards and one-accepted-try rule, and is not sweepable. Only a stop in the
   device's reports confirms it; until then no confirmed undo exists.
9. **Still excluded:** `0x4A` and `CMD_WRITE_PRIVATE`, as ADR 0024 decides.

## Consequences

- The probe's documentation says plainly that it changes the device, that no confirmed undo is
  known, and to use it on one device and one event at a time.
- A battery motor that was subscribed may spend battery on reports; the first run is limited to one
  device on purpose.
- Decoding the reports, an entity for them and automatic subscription are separate, later decisions
  that need captured reports first.
- The recorder is a general tool: any frame a registered device sends that nothing handles shows up
  on the diagnostic sensor, so it also surfaces commands this project has not met yet.

## Amendment (2026-10): reports arrive as `0x71`

The Decision above expects a subscribed device to report with `0x70`. The first device to take a
subscription, an RS100 Solar io in issue #156, answered `0x73 01 20 05` with a `0x71` instead:
`05 60 20 05 05 06 00 00 00 00` (corpus `somfy_rs100_probe_subscribe_event_sx1262`). Every `0x71`
in the corpus has the same header: a flags byte, a status byte, a two-byte event id, the length of
the event data, then the event data. Position updates carry event `0x100A`, this answer carries the
subscribed `0x2005`. Before this change the hub decoded every `0x71` as a position and dropped this
one as too short, so a report would never have reached the sensor.

A device event report is therefore a `0x70`, or a `0x71` whose length byte matches the frame and
whose event is not `0x100A`. Point 5 applies to it unchanged with one difference: a `0x71` addressed
to the hub is authenticated and acknowledged with `0x72`, as every status update is, before it is
recorded. It is still never applied to device state. A `0x71` of any other shape keeps the position
path, so a layout never captured is not reinterpreted. The probe's own answer is recorded as the
first report.
