# ADR 0072: Diagnostic probes are their own collaborator

<!-- doxygen-label: adr0072 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0024](0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md) (the
"Isolation from the status decoder" section: the guarantee no longer rests on `ManagementActions`
not being a friend of the hub).

## Context

ADR 0024 keeps a probe reply away from the status decoder because `ManagementActions` cannot reach
`IOHomeControlComponent::update_device_status_()`: the method is `protected` and the class is not a
friend. That held, but the probes had grown to about 670 lines of a 1600-line file that also holds
the user-facing actions, so the property was true of a large class that has a hub pointer and uses
it for many other things.

## Decision

The probes are a class of their own, `DiagnosticProbes` (`diagnostic_probes.h`). `ManagementActions`
owns one instance, keeps the single registration table (the two probe rows call into it) and
forwards `probe_device()` and `probe_sweep()` to it, so the hub's forwarders are unchanged.

`DiagnosticProbes` holds no hub pointer and includes no hub header. It receives three calls
(`DiagnosticProbesHub`): whether probes are enabled, the radio for the capture log line, and the
recorder of a device event report. None of them applies a status to a device record. Result type and
argument helpers that both classes use live once in `management_helpers.h`.

The isolation guarantee of ADR 0024 now rests on the class boundary and the narrow interface: a probe
reply stays raw because the class that handles it has nothing to hand it to.

## Consequences

- A new probe cannot reach the status decoder by accident; it would have to add a fourth call to
  `DiagnosticProbesHub`, which shows in review.
- `management_actions.cpp` shrinks to the user-facing actions and the registration.
- `ManagementActions` builds the three calls from its hub pointer; the hub itself is unchanged, and
  the probe tests drive the same hub forwarders as before.
- The include-graph rules need no new allowlist entry: `diagnostic_probes.cpp` does not reach
  `hub_core.h`.
