# ADR 0040: A low-power device's start preamble follows its wake belief

<!-- doxygen-label: adr0040 -->

**Status:** Proposed · **Recorded:** 2026-09

Amends [ADR 0029](0029-start-preamble-is-a-property-of-the-target.md). Becomes Accepted once a
real low-power receiver confirms mid-travel `stop` and status polls on hardware.

## Context

ADR 0029 made the start preamble a property of the target's power class: a device declared
`low_power: true` gets `LONG_PREAMBLE` (1024 bytes, ~213 ms) on every directed start frame, because
a duty-cycled receiver needs a long carrier to catch its next wake-up window.

That rule is right for a receiver that is asleep and wrong for one that is awake. A VELUX solar
roller shutter that is moving — or has just moved — is not duty-cycling: it is listening
continuously, and it ignores the 1024-byte preamble the same way the always-alive VELUX receivers
in ADR 0029 do. It answers the short one. Hardware showed both halves on the same motor: with
`low_power: false` a `stop` mid-travel works, but the motor can then not be started from rest; with
`low_power: true` it starts from rest, but a `stop` or status poll sent while it moves goes
unanswered. Neither fixed setting serves both states, and the state changes within a single
command's lifetime.

So the preamble that will be heard depends on the target's *state*, which the hub can partly infer:
it knows when it commanded a move, when a status said "not stopped", and when it last heard the
device at all.

## Options considered

1. **Keep the fixed rule.** Simple, and correct for a sleeping receiver. A moving receiver never
   hears a `stop`, which is exactly the command a user needs to work mid-travel.
2. **Flip the fixed rule to the short preamble.** Fixes `stop`, breaks every start from rest.
3. **Send both on every exchange.** A short try and a long try back to back, whatever the belief.
   Costs airtime on every command to serve the minority of exchanges that hit an awake receiver.
4. **Order the tries by a per-device belief (chosen).** Lead with the preamble the receiver is most
   likely to hear, and keep the other one in the retry sequence so a wrong belief costs one try.

## Decision

**D1 — A wake belief per low-power device, derived from evidence the hub already collects.** Three
levels, `ASLEEP`, `MAYBE_AWAKE`, `AWAKE` (`decisions::WakeBelief`), computed by one pure function
(`decisions::wake_belief()`) from two stamps and the request:

- `AWAKE` for a `stop` request (it is only ever sent to a receiver that is moving), or while
  moving evidence is younger than `LOW_POWER_MAX_TRAVEL_MS`;
- `MAYBE_AWAKE` while the last frame heard from the device is younger than
  `LOW_POWER_AWAKE_HOLD_MS`;
- `ASLEEP` otherwise, including for a destination the hub has no record of.

Moving evidence (`IoDevice::last_moving_evidence_ms`, written only by `note_moving_evidence()` and
`clear_moving_evidence()`) is stamped when a movement command is accepted by the device (including
one that gets no ack to decode, and on devices with `optimistic_state: false`), when a decoded
status says the device is not stopped, and when a linked remote's movement command is overheard. It
is cleared when the device is observed stopped, told to stop, or heard to be stopped by a remote.
It is deliberately *not* stamped by the hub's own optimistic prediction: that is applied before the
command is sent, so it would make a resting receiver look awake to the very command that starts it.
A command that failed stamps nothing. The other input, `last_seen_ms`, already existed for link
health and is stamped by every frame from a registered device; pairing now stamps it at
registration too.

**D2 — The belief orders the tries of a multi-try exchange; it never removes the wake-up preamble.** Per try, with
short = `normal_start_preamble` and long = `LONG_PREAMBLE`:

| Belief | Try 1 | Try 2 | Try 3 |
|---|---|---|---|
| `AWAKE` | short | long | short |
| `MAYBE_AWAKE` | short | long | long |
| `ASLEEP` | long | long | long |

Every plan sends the wake-up preamble at least once, and the plans apply only to an exchange allowed
more than one try. A scheduler-owned status poll is allowed one at most ladder slots (its backoff
ladder is its retry); it keeps the fixed wake-up preamble, because a single try would stake the whole
poll on the belief, and a wrong one — a receiver that looks awake but has finished a short move and
gone back to sleep — costs a failed poll, an incremented failure streak and a backoff. The frame bytes never change between tries;
only the transmitter's preamble length does. `ExchangeEngine::send_and_receive()` resolves the
belief once per exchange (`plan_request_preamble_()`), so it is stable across the tries and the
evidence is looked up once.

**D3 — The engine decides, the hub supplies evidence.** The hub installs a lookup
(`ExchangeEngine::set_wake_evidence_provider()`) that returns a device's stamps; the engine derives
the belief, checks the tuning switch, and picks the preamble. The whole decision — switch, evidence,
STOP shortcut, plan — sits in one place and is testable without a hub.

**D4 — Scope.** Only a low-power *start* frame sent by `send_and_receive()` without an explicit
preamble override is affected. Always-alive devices, non-start frames, the 0x3D challenge response,
pairing's directed frames (they pass an override), the roll-call and the key-extraction responder
are unchanged. `request_preamble_for()` keeps its single-shot asleep/always-alive rule for callers
that send once.

**D5 — One diagnostic switch.** The `low_power_wake_belief` tuning parameter, on by default. Off
restores the fixed `LONG_PREAMBLE` on every try to a low-power device, byte- and
preamble-identical to the behaviour before this ADR. It is documented as a diagnostic: if a
low-power device got worse after updating, set it to `false` and report.

## Consequences

- **Amends ADR 0029.** The preamble is a property of the target *and its state*: `low_power`
  still governs `CTRL1_LOW_POWER` and whether a wake-up preamble exists in the plan at all, but
  which preamble leads is now a runtime belief. ADR 0029's `LONG_PREAMBLE` for a declared low-power
  target holds for a receiver believed asleep.
- **An exchange never bets on one preamble.** A wrong belief costs one try (~0.4-0.9 s of listen
  and retry gap), not the exchange, and no plan drops the wake-up preamble entirely: a single-try
  exchange does not use a plan at all.
- **`stop`, reversals and the middle of the poll ladder can now land mid-travel.** The first settle
  poll (a single try, seconds after the command, inside the manoeuvre) and the far tail of the ladder
  keep the wake-up preamble, exactly as before this ADR, so a moving receiver still misses that first
  poll. The slots that get three tries (roughly t+8 s to t+53 s) lead with the short preamble.
- **Airtime goes down for an awake receiver.** A short first try is ~200 ms shorter on the air
  than a long one (32 vs 1024 bytes at 38.4 kbit/s).
- **The two windows are first estimates.** `LOW_POWER_MAX_TRAVEL_MS` (120 s) and
  `LOW_POWER_AWAKE_HOLD_MS` (30 s) are guesses field logs will correct; the exchange-failure log
  line carries `belief=` (the belief, or why none applied: `off`, `single_try`, `not_low_power`,
  `override`) and `last_preamble=` so that data exists, and the config dump at boot states whether
  the switch is on.
- **Not done here.** The ASLEEP plan stays long/long/long; a `long/short/long` variant is a
  follow-up only if logs show a receiver that wakes and then ignores the long preamble. The
  discovery sequence for a not-yet-paired device is a separate decision.
