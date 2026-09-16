# ADR 0039: Pairing sends a discover-confirm (0x2C) and tolerates no answer

<!-- doxygen-label: adr0039 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

Every real controller this project has captured sends `CMD_DISCOVER_CONFIRM` (0x2C) directly to a
device between that device's discovery response (0x29) and the controller's own `CMD_KEY_INIT`
(0x31): a real VELUX KLR200 talking to a KUX100, a Somfy TaHoma joining an existing VELUX SSL
system, and every hub this project's own key-extraction responder has answered (VELUX KIG300,
KLR200, KLR300; Somfy TaHoma; a Somfy Connectivity Kit). `PairingEngine::discover_and_pair()` was
the only controller in the corpus that skipped straight from discovery to key-init.

Somfy's own tolerance for the missing step is partial evidence, not proof it is harmless
everywhere. Two families this project has hardware-validated (Izymo, RS100) pair fine without it.
A third capture (Sunilus) does *not* show tolerance: it answers `CMD_KEY_INIT` with a 0x3C, then
rejects the following 0x32 with an undocumented `0xFE 0x80`, deterministically, in a sequence that
also has no 0x2C — whether the missing step causes that specific rejection is unconfirmed, but it
rules out treating "Somfy tolerates it" as a settled fact across every Somfy family.

Timing also differs sharply between real controllers and this project's own flow. Real controllers
leave 0.67-1.86 s between 0x29 and their 0x2C, and a further 4.6-8.1 s between that 0x2C and their
0x31 — which they appear to spend re-broadcasting 0x28 to look for more devices, something a
single-device discovery phase has no equivalent of. This project's own 0x29-to-0x31 gap has been
as short as 9 ms (Izymo, which pairs successfully) or 237 ms (a VELUX SSL that stayed silent afterward). For a
VELUX-class device, "too early" cannot be ruled out as a contributing cause of that silence.

One VELUX SSL account also shows the device issuing a 0x3C challenge after the controller's 0x32,
something the existing slow-turnaround confirm wait (`PairingEngine::wait_for_key_confirm_()`) had
no branch for: it classified any non-0x33 reply as a refusal and gave up the whole wait.

## Decision

**D1 — Universal, tolerant, one mode knob.** A new tuning enum, `pairing_discover_confirm`
(`DiscoverConfirmMode` in `tuning_config.h`): `skip` (kill switch — no 0x2C, no pause), `send`
(default), and `send_with_ack`. Not a bool reusing
`pairing_discovery_ack_capable`: that knob's contract is scoped to the discovery broadcast only, in
five places across the codebase, and overloading it would couple two independent experiments.
Values avoid `on`/`off`, which YAML 1.1 parses as booleans. The step never fails a pairing attempt
in any non-`skip` mode — a refusal, a timeout, and an ACK are all just different log lines on the
way to the same next phase.

**D2 — Frame shape.** `create_discover_confirm()` builds the 0x2C: a start frame, no payload,
`CTRL1_LOW_POWER` set from the target's own discovered power-save class (mirroring every corpus
hub, which does this with no exceptions on record), and `CTRL1_ACK` set only for an always-alive
target in `send_with_ack` mode — a low-power target's 0x2C is `CTRL1_LOW_POWER` alone in *both*
`send` modes, byte-identical to a real TaHoma's frame to a VELUX SSL. The always-alive `send`
shape (`CTRL1` all-zero) has no corpus precedent; it is the default because `CTRL1_ACK` on outbound
frames has silenced real Somfy awnings before (see `pairing_discovery_ack_capable`'s own history),
and the hardware validation below confirmed the same for this frame: a Somfy Izymo dimmer answers
`CTRL1=0x00` and ignores `CTRL1=0x10`. `send_with_ack` keeps the hub-observed `0x10` shape
available for an always-alive device from an ecosystem that expects it. Preamble follows the same start-frame
rule as any other directed frame (ADR 0029): `LONG_PREAMBLE` iff `CTRL1_LOW_POWER`, the
runtime-tunable `normal_start_preamble` otherwise — this ADR adds no new preamble rule, it just
routes the discover-confirm frame through the existing one
(`ExchangeEngine::request_preamble_for()`, promoted from a private helper so this new caller can
reach it without a second copy).

**D3 — Waiting for 0x2D.** Up to `PAIRING_DISCOVER_CONFIRM_TRIES` (3) tries, `1500` ms each. A
failed transmit counts as a silent try, the same convention `wait_for_key_confirm_()` already
uses for a failed 0x32. Tries 1 and 3 hold the request channel, matching every other unicast
pairing wait in this project on the expectation that a unicast reply comes back on the request
channel it answers, and every 0x2D the Somfy Izymo dimmer sent during validation did. But the one
VELUX-system sample on record (a hopping monitor, a weak source) logged a 0x2D on a *different*
channel than its own 0x2C, so the middle try hedges by rotating across all three channels until a
VELUX device's reply channel is measured.
**This deviates from ADR 0028** ("channel policy is a property of the frame, not of the chip" —
one policy per reply shape): here, one reply shape gets two different policies across its own
retry tries — only try 2, not tries 1 and 3, actually deviates from the hold-the-request-channel
convention. Treated as a temporary exception pending field data on which channel a real 0x2D
actually favors, not a new general pattern.

**D4 — Pause before 0x31.** `pairing_key_init_delay_ms`, default `300` ms
(`PAIRING_KEY_INIT_DELAY_MS`, `proto_timing.h`), applied once after the step for every outcome
except `skip` — including a silent `NO_REPLY`, so there is one rule instead of a per-outcome one.
300 ms has no capture precedent; real controllers leave several seconds here, but they appear to
spend it on repeated 0x28 broadcasts, which a single-device flow has no reason to copy. The Izymo
pairs with both 300 ms and 5000 ms, so the default stays short and the knob covers a device that
needs a later 0x31. A multi-second value of this knob would need to run beyond a
single `delay()` without tripping the loop-task watchdog, hence `delay_feeding_wdt()` — feed,
sleep up to 1000 ms, repeat — rather than a bare `delay()`.

**D5 — The device may challenge the key transfer.** On the fast-turnaround path (SX1276),
`ExchangeEngine::handle_authentication_()` already answers a 0x3C over the 0x32 with no change
needed. On the slow-turnaround path (SX1262, LR1121), `wait_for_key_confirm_()` gains a
`CHALLENGE` disposition alongside its existing `CONFIRM`/`REFUSE`: a well-formed 0x3C from the
device is answered with a 0x3D (`ExchangeEngine::answer_challenge()`, shared with the normal
inbound-challenge path so the 0x3D transcript rule and its preamble choice have exactly one
owner), then one fresh confirm-wait window is given to catch the resulting 0x33 — a full window,
not whatever remained of the first one, since a late challenge would otherwise leave almost nothing
for the 0x3D round-trip. A second challenge in that fresh window ends the try without spending it
on a fruitless third listen; the next try re-sends 0x32 and draws a fresh challenge. This is
strictly additive: any non-0x33, non-refusal reply here was previously an unhandled case that fell
through to a refusal.

**D6 — Observability.** Two new `PairingState` values, `TX_DISCOVER_CONFIRM` and
`WAIT_DISCOVER_CONFIRM`, both under the frozen `v1;` result-string's longest-phase-name budget.
`set_phase()` (telemetry) fires once per state for the whole step, not once per try — the step can
spend up to three transmits, and the pairing advisor's telemetry buffer is a fixed 32 events;
spending several of them on exactly the retries a stuck pairing needs diagnosed would be
counterproductive. `record_debug()` (not telemetry) still runs every try.

## Consequences

- **Added latency.** About +0.3 s (the pause) plus the device's own reply latency when a 0x2D
  comes back on the first try — a quick, already-successful attempt (documented example:
  `dur_ms=842`) stays quick. With no answer at all: up to roughly 4.8 s for an always-alive
  target, 5.4 s for a low-power one (three 1.5 s tries, the longer one at `LONG_PREAMBLE`'s
  wake-up preamble), stacked on top of the existing key-exchange worst case. A slow or failed
  attempt already exceeds ESPHome's 2550 ms "took a long time for an operation" threshold on its
  own; this step's worst case can now push even a quick success past that threshold too.
- **The kill switch is real, not decorative.** `pairing_discover_confirm: skip` sends no 0x2C and
  applies no pause. Any regression traceable to this step has a one-line documented way back.
- **The default always-alive frame shape (`send`, `CTRL1` all-zero) is not the hub-observed one.**
  No corpus hub sends it, but it is the shape a Somfy device answers; the hub-observed `0x10` shape
  gets no answer from Somfy, so it stays opt-in (`send_with_ack`) for devices that need it.
- **Corpus replay needs a mode per capture.** `tests/corpus_pairing_replay_test.cpp` sets
  `pairing_discover_confirm` per capture (`skip` when it has no captured 0x2C, `send`/
  `send_with_ack` matching the captured frame's own `CTRL1_ACK` bit otherwise) so the byte-exact
  replay check also pins the new framing once a capture exercises it. A capture that shows a real
  0x2C but no corresponding 0x2D cannot replay through this step cleanly — the wait would swallow
  whatever comes next as an unrelated frame — so such a capture needs its own 0x2D or must be
  excluded from that suite with that reason recorded.
- **A device that dislikes 0x2C outright is not yet known to exist, but is not ruled out either.**
  Mitigated the same way as `pairing_discovery_ack_capable`: a documented tuning escape hatch, not
  a code branch that guesses which devices need it.

## Validation

Hardware-validated on a Somfy Izymo dimmer (always-alive) with an SX1262 hub, two pairings per
configuration, all eight paired and the device was controllable afterwards:

| Configuration | 0x2D answered | Notes |
|---|---|---|
| `skip` | — | baseline, identical key exchange |
| `send` | 2 of 2 tries, 12–14 ms, request channel | answered on try 1 both times |
| `send_with_ack` | 0 of 6 tries | still paired, after the ~4.85 s no-answer wait |
| `send` + 5000 ms pause | 2 of 3 tries, 12–23 ms, request channel | one try-1 miss, answered on try 2; late 0x31 accepted |

The key exchange itself (0x31→0x3C→0x32→0x33) was identical in every configuration. Not yet
validated on a VELUX device, which is what the step was added for.

See also ADR 0028 (the per-try channel-policy convention try 2 deviates from) and ADR 0029 (the
start-preamble rule this reuses rather than re-implementing).
