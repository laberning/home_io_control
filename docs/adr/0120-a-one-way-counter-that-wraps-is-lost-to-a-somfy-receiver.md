# ADR 0120: A one-way counter that wraps is lost to a Somfy receiver

<!-- doxygen-label: adr0120 -->

**Status:** Accepted, amends [ADR 0114](0114-a-one-way-counter-resumes-from-its-persisted-value-across-the-wrap.md) · **Recorded:** 2026-11

## Context

ADR 0114 left open what a device does with a one-way sequence number that steps across the 16-bit wrap (65535 to 0).
A bench run on an enrolled Somfy Izymo dimmer (mains), with the identity's counter set by a bench-only action and each
probe checked by a two-way read, measured the rule: the device obeyed every higher number tried (+1 up to +32000),
and ignored the same number, a lower one, and a step of +2000 across the wrap (64841 to 1305). After each ignored
probe, the next number above the last obeyed one was obeyed again.

## Decision

- The hub's docs and comments state the measured rule for a Somfy receiver: any higher number is accepted, compared
  as a plain number; a step across the wrap never is.
- ADR 0114's boot rule stays: the hub never sends a number it already sent. Its sentence that the device's behaviour
  after a wrap "is not measured" is replaced by this: on a Somfy receiver an identity whose counter wraps is lost to
  that device and needs a new identity.
- The hub keeps its own automatic forward skips small: the persistence block times its reboot headroom stays below a
  ceiling of 100, a bound the hub sets for itself so that a receiver with a stricter rule than the Izymo's is safe too.
  The measured rule gives them ample room.
- VELUX receivers are not measured; the docs say so.

## Consequences

- A user learns from the docs that a one-way identity has at most about 65 000 commands on a Somfy receiver (fewer
  when `initial_sequence:` started it higher), and that afterwards it needs a new address, enrolled again.
- Whether the hub should warn as the counter nears 65535 is left to a later change.
