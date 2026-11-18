# ADR 0083: Heating and hot water live in their own collaborator

<!-- doxygen-label: adr0083 -->

**Status:** Accepted, amended by [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md) (the opt-in register list is derived from the reading table) · **Recorded:** 2026-11

Supersedes [ADR 0033](0033-heating-send-path-bypasses-the-cover-machinery.md) in full (its remaining
points are restated below). Amends [ADR 0004](0004-hub-collaborator-decomposition.md) (the collaborator
list gains `HeaterController`). [ADR 0082](0082-heater-exchanges-share-the-exchange-bookkeeping.md)
stands: heater exchanges keep the shared exchange bookkeeping.

## Context

Reading and commanding heaters and water heaters grew inside the hub class: register reads and writes,
the water heater's operating-mode and duration writes, the regular heater polls with their model check
and programme period, and the decoding of a heater's and a water heater's status answer. Together they
made up more than a third of the hub's operations code and over twenty of its declarations, so every
heater change edited the largest and most-changed files of the component. ADR 0033 explained why the
heater write path bypasses the cover's send-and-settle helper; its premise that heaters have no
read-back was already removed by ADR 0054, and ADR 0082 moved heater exchanges onto the shared
bookkeeping.

## Decision

1. **`HeaterController` owns heating and hot water**: the register heater's writes and register reads,
   the water heater's reads and writes, the heater and water heater polls, and the decoding of their
   status answers. It lives in `heater_controller.*`, outside the `hub_*` files, so the include graph
   keeps it away from `hub_core.h` and `hub_internal.h`.
2. **It holds no hub pointer.** As for `DiagnosticProbes` (ADR 0072), the hub hands it the node id,
   the initialised flag, the radio pointer, the device registry, the status poll policy, the exchange
   engine's debug information, and a small set of injected calls: run an exchange with the shared
   bookkeeping, run an EXECUTE through the hub's execute funnel, run a status read, arm a timer keyed by
   the device, queue a status request, and back off a failed poll.
3. **The status read reaches the status decoder.** Unlike a diagnostic probe, a heater or water heater
   status read is an ordinary status read: the hub decodes the answer and hands the heater part of it
   back to the controller's stateless decoders.
4. **The hub keeps one-line forwarders** for the calls the climate and water heater entities and the
   `heating_control` action make, and nothing else of heating.
5. **ADR 0033's remaining points hold, restated:** a private-register write or read does not pass
   through the cover's status decoder and gets no poll backoff from it; the climate entity publishes
   commanded values only after a successful send and uses no ADR 0030 optimistic overlay; a capability
   check from the type table gates every heating entry point.

## Consequences

- A heater or water heater feature is added and tested in the controller and its test fixture, without
  a hub object.
- No frame on air and no cadence changes.
- Three small helpers that the heater code shares with the hub (logging a rejected operation, stamping
  link health, clearing the last result code) live in `entity_helpers.h`, which both may include.
- Until entities get a narrow port of their own, they reach the controller through the hub's forwarders.
