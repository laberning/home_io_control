# ADR 0049: A rain limitation is derived from observed status, never predicted

<!-- doxygen-label: adr0049 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

The Active Issue sensor reports `LIMITATION_BY_RAIN` when a command draws a `CMD_ERROR_RESP` with
that result code. VELUX windows never send one for rain. Two reporters (issue #98) see the same
thing: while the rain protection is active, an `open` is accepted (the authentication challenge is
answered, no error comes back) and the window quietly stops at its ventilation position. The hub
predicts "open, 0 %", the next status poll says "stopped at 93 %", and nothing explains the gap.

The window does say what is going on in the ordinary status reply to the hub's poll. The reply
carries the last-command record, whose Command Originator reads `rain sensor` (`0x02`) after the
window closed itself for rain, and a stopped position at the ventilation opening. The KLF 200 API
specification v3.18 (section 8.1) describes the rain sensor as talking directly to the window
opener, so a gateway only learns of a rain closure by asking.

What a window reports in its last-command record *right after a clamped open from the hub* has not
been observed: it may keep the rain originator or switch to the hub.

## Options considered

- **Treat any disagreement between prediction and observation as a rain limitation.** Rejected: an
  end stop, an obstacle or another protection produces the same disagreement.
- **Record the limitation when the command is sent.** Rejected: that is a prediction, and ADR 0030
  keeps predictions apart from observations.
- **Write the derived state into `last_result_code`.** Rejected: that field means "the device sent
  an explicit result", and an explicit `CMD_ERROR_RESP` has to keep winning over an inference.
- **Poll the window on a timer to keep the state fresh.** Rejected: more radio traffic on a shared
  band for a state the existing polls already reveal.
- **Derive a separate flag from observed replies with two evidence paths** (chosen).

## Decision

- `IoDevice::limited_by_rain` is a derived flag, separate from `last_result_code`. It is
  recomputed from every trusted status reply (a poll reply or a device-initiated status update),
  never from an execute acknowledgement and never from a prediction.
- `decisions::rain_limitation_rule()` applies two rules in order:
  1. The device names the rain sensor as the originator of its last command.
  2. Rain evidence is fresh (`RAIN_EVIDENCE_HOLD_MS`), the device reports itself stopped, both the
     hub's predicted target and the observed target are known, and they differ by more than the
     position tolerance: an accepted position command was clamped.
- `IoDevice::last_rain_evidence_ms` is the memory for rule 2. A status reply that names the rain
  sensor refreshes it; a stopped device sitting where the hub predicted clears it.
- The Active Issue sensor shows the explicit result name when there is one, otherwise
  `LIMITATION_BY_RAIN` while the flag is set.
- Every change of the flag is logged at DEBUG with the rule and its inputs, so field reports can
  settle what a window reports after a clamped open.
- Unauthenticated frames are still never applied to the device record (ADR 0022), and the probe
  path stays isolated from the status decoder (ADR 0024).

## Consequences

- **No extra radio traffic.** The flag is only as fresh as the last status reply. A window that
  closes itself for rain is noticed on the next poll or command, not at once.
- **Rule 2 depends on an unobserved behaviour.** If the window keeps naming the rain sensor after
  a clamped open, rule 1 alone carries the case and the memory is never needed.
- **`RAIN_EVIDENCE_HOLD_MS` is an estimate** (two hours). Too short, and later `open` attempts in a
  long shower are no longer labelled; too long, and an unrelated clamp after the rain has stopped
  is mislabelled.
- **It is an inference.** The flag means "the last move was by the rain sensor, or an open was
  clamped after one", not "it is raining".
