# ADR 0030: A hub prediction is kept apart from a device observation

**Status:** Accepted · **Recorded:** 2026-08

## Context

To give the Home Assistant UI immediate feedback across the queue-dispatch and
exchange gap, the hub writes an *optimistic* state the moment a command is
issued: a cover asked to close shows "closing" before any frame has gone out.
The same happens when the hub overhears a 1W remote press for a linked device.

That optimistic value used to be written straight into the same `IoDevice`
fields a decoded status frame writes — `target`, `tilt`, `is_stopped`. One
field, two possible authors, no way to tell a guess from a report. Three
symptoms followed directly from that conflation:

- **A guess could never be withdrawn.** Withdrawing one might destroy a real
  observation that happened to be sitting in the same field, so
  `run_execute_operation_()` never tried. A failed command left `target` set
  and `is_stopped == false` forever, and the HA cover animated travel that was
  not happening — indefinitely, since only a frame from the device could
  settle it.
- **Tilt had no inverse at all.** The optimistic tilt overwrote the observed
  slat angle outright; there was nothing left to restore. Clearing it to
  `UNKNOWN_POSITION` did not help either — the cover entity skips an unknown
  tilt, so it kept publishing the stale guess.
- **A latent hazard on the STOP path.** The STOP handler cleared `target`
  unconditionally, so it would discard a genuine wire-reported target whenever
  one was present.

## Decision

Predictions get their own storage: an `OptimisticState` overlay on `IoDevice`
holding `{target, tilt, motion}`, where `motion` is tri-state
(`NONE / MOVING / STOPPED`). The observed fields then mean exactly what their
names say — "this is what the device last told us" — and are never written by a
guess. Every consumer reads a derived effective value: `effective_target()`,
`effective_tilt()`, `effective_is_stopped()`, each preferring the prediction
when one stands and falling back to the observation otherwise.

Three rules govern the overlay, and nothing else:

- **A prediction is written only by `apply_optimistic_*`** (an HA command via
  `IOHomeCover::control()`, or an overheard 1W press via
  `apply_optimistic_linked_state_()`).
- **A prediction is withdrawn by `DeviceRegistry::rollback_optimistic()`** when
  the command that produced it fails. `run_execute_operation_()` wraps
  `try_execute_operation_()` and calls it on every false return — an
  unregistered device, a guard rejection, a builder failure, a silent timeout,
  or an explicit `CMD_ERROR_RESP`. `SUCCESS_UNCONFIRMED` on a `CMD_EXECUTE` is
  *not* a failure (the device authenticated the request, so it has the
  command), so the prediction stands there. The 1W producer has no failure to
  attribute — the hub commanded nothing — so nothing rolls its predictions
  back; they are superseded by the poll `schedule_device_polls_()` arms.
- **A prediction is superseded, per axis, by the observation that replaces
  it** — a decoded position clears the position prediction, a decoded tilt
  clears the tilt prediction. This is what makes snapshot-and-restore
  unnecessary: there is never a stale saved value to restore, because a
  prediction stops existing the moment real data covers it.

### Why supersede is per-axis, not global

This is the non-obvious constraint, and the one a future change is most likely
to break. `apply_private_response_status()` is called with
`trust_position = false` for the immediate reply to our own `CMD_EXECUTE`,
because that ack has been observed on real hardware echoing *pre-command*
target/current values. A blanket "any observation clears the overlay" rule
would snap the cover back to that stale pre-command state for the seconds until
the follow-up poll. The per-axis rule preserves the execute-ack path exactly,
because that path decodes no position and so clears nothing. The same reasoning
covers tilt: the unsolicited `STATUS_UPDATE` (0x71) path decodes position but
not tilt, so it clears the position prediction and correctly leaves the tilt
prediction standing — a tilt observation from an earlier poll would be no more
confirmed than the guess it would replace.

### Why `normalize_stopped_state()` is exempt

It reads and writes observed fields only, never `effective_*()`. Its job is
"the device said stopped but its own reported target and current disagree" —
a statement about observations. Feeding it an effective value would push a
prediction back into an observed field and reintroduce the conflation this ADR
removes. One consequence: on the execute-ack path it no longer flips
`is_stopped` back to false using the hub's own commanded target (which is no
longer in `dev.target`); `effective_is_stopped()` carries that responsibility
now, which is where it belongs. `log_status_update()` is exempt for a related
reason — it is a protocol log line emitted right after decoding a frame, so it
must report what arrived on the wire, not what the hub predicts.

## Relation to ADR 0019

ADR 0019 declares what the protocol cannot report; a prediction is the exact
opposite move — the hub's own guess at a fact the wire has not yet carried.
Both exist because the radio does not carry the fact at the moment it is
needed. ADR 0019 keeps the guess *out* of a field the user must trust
(inversion, dimmability); this ADR keeps the guess *out* of a field a later
frame must be free to correct. `optimistic_state: false` remains the
per-device opt-out from ADR 0019's "safe default, explicit opt-out" shape: a
device configured that way fills no overlay, so `rollback_optimistic()` and the
supersede rule are both no-ops for it.

## Consequences

- **A failed position or tilt command reverts the entity** to its last
  reported position instead of animating forever. An awning refusing with
  `LIMITATION_BY_WIND` stops animating too, since `handle_error_response_()`
  also returns false.
- **A failed STOP falls back to the observed movement state** rather than
  claiming the device stopped — more truthful, since the STOP did not arrive.
- **Two small ack-path poll-scheduling changes, in opposite directions, both
  practically nil** because `run_execute_operation_()` /
  `arm_execute_confirmation_poll_()` re-arm the confirmation poll immediately
  regardless. (1) When an execute ack reports stopped and the commanded
  target already equals the last observed position, `effective_is_stopped()`
  now returns false for one more poll (motion is still `MOVING`), so poll
  tracking survives where it used to be cleared. (2) When a STOP ack still
  carries a "moving" bit while a `Motion::STOPPED` prediction stands,
  `effective_is_stopped()` reads stopped, poll tracking is cleared, and a
  possibly-shorter hint-derived deadline is skipped. Both are called out so a
  future `hub_status_test` change here is recognised, not debugged as a
  regression.
- **The "animates forever" defect is closed only for commands that fail.** An
  *accepted* command (`SUCCESS_UNCONFIRMED`, or a `trust_position = false`
  ack) leaves `Motion::MOVING` standing, and if the device then goes silent
  nothing withdraws it — only a later observation supersedes it. Making an
  accepted-then-silent device fall back to "unknown" belongs to the
  reachability / availability work (see `device_availability_analysis.md`),
  deliberately out of scope here.
- **A 1W prediction standing when a later hub command fails is withdrawn along
  with it**, because `rollback_optimistic()` clears the whole overlay. Safe by
  construction — withdrawing always falls back to observations, never to
  another guess — and cheaper than tracking per-prediction provenance for a
  case this rare.
- **12 bytes per device** for the overlay (two floats, one byte, padding).
- Nothing on the wire changes: no frame builder, no decoder, no corpus.
