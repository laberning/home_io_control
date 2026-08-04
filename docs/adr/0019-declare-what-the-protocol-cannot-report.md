# ADR 0019: What the protocol cannot report is declared, not guessed

**Status:** Accepted · **Recorded:** 2026-08

## Context

Several things the component needs to know are not discoverable over the
radio. The protocol carries no capability descriptor: nothing says whether a
light supports intermediate brightness or only on/off, whether a cover's
position values run inverted, or whether a device will volunteer status
updates without being polled.

Guessing wrong here is not cosmetic. Treating a binary light as dimmable sends
values the device may interpret unpredictably. Getting inversion wrong means
"open" closes someone's shutter.

## Decision

Where a property is genuinely unknowable from the radio, it is an explicit
YAML declaration rather than an inference. Where a *safe* default exists, it
is the default, with an explicit override available.

Concretely, three shapes appear:

- **Explicit opt-in, no guess.** Brightness support defaults to off. The
  protocol gives no signal at all, so the component will not infer one; a user
  with a dimmable device says so.
- **Learned default, explicit override.** Position inversion falls back to a
  per-device-type profile, which the user can override outright when their
  particular device disagrees.
- **Safe default, explicit opt-out.** Optimistic state updates default on, and
  can be turned off for a device where predicting the outcome proves wrong.

The same principle covers the device type itself: it can be declared in YAML,
and is otherwise learned from radio metadata when the device happens to
volunteer it — a declaration always wins over a guess.

## Consequences

- A user with an unusual device has a way to describe it, instead of waiting
  for the component to add detection that the protocol cannot support anyway.
- More YAML options than a self-describing protocol would need, and each is
  one more thing to document and validate. That is the accepted cost of not
  guessing about physical devices that move.
- Defaults are chosen for the *least harmful* wrong answer, not the most
  common device. Binary-only is the safe assumption for a light because
  sending on/off to a dimmer is harmless, while the reverse is not.
- When a future capability turns out to be genuinely detectable from radio
  metadata, detection can be added as a new fallback layer beneath the
  explicit option, without breaking configs that already declare it — the
  declaration keeps winning.
- A related consequence shows up in behavior tuning rather than capability:
  a user's configured poll interval is likewise treated as authoritative
  against the device's own opinion (ADR 0017).
