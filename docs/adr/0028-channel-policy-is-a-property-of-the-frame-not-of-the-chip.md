# ADR 0028: Channel policy is a property of the frame, not of the chip

**Status:** Accepted · **Recorded:** 2026-08

## Context

Every blocking wait in this project — six of them, across `ExchangeEngine` and `PairingEngine`
— is waiting for one of exactly two things, and which one determines where a reply can arrive:

- **A unicast reply to a unicast request** comes back on the channel the request went out on.
  Measured on every unicast pairing reply captured: the fresh-install `0x33` key-confirm in all
  three dimmer pairing captures across three chips (4–41 ms turnaround), every `0x3C` challenge,
  both field-logged `0xFE` error replies (20 ms), and 12 of 12 hardware pairing trials across two
  chips. Zero of these ever arrived on a different channel than the request.
- **A reply to a broadcast** does not come back on the request channel. Measured on the
  broadcast roll-call: 1 reply of 149, against the ~1 in 3 an even split would give.

Before this refactor, that distinction was expressed six different ways, in six separately
evolved wait loops, and two of the six drivers had independently rediscovered it and encoded it
as a chip-specific number instead of a loop-specific policy:

- `SX1262_EXCHANGE_RESPONSE_WAIT_SLICE_MS = 90` — the driver's own comment: 50 ms "was short
  enough that the controller could hop away from the request channel just before the device
  emitted its post-auth reply."
- `LR1121_EXCHANGE_RESPONSE_WAIT_SLICE_MS = 600` — set above the largest response-wait budget
  "so this driver never hops away from the request's channel while waiting for a reply — real
  replies for this device only ever arrive on that same channel anyway."

Both comments describe the same protocol fact — a unicast wait should never hop — worked around
at the driver layer because the loop itself offered no way to say it. The workaround also leaked
into the one loop that genuinely needs to rotate: `collect_broadcast_responses()` reused the same
per-chip accessor, so on LR1121 the broadcast roll-call inherited "never hop" from a constant that
existed to serve an unrelated unicast loop — about three dwells across a 2000 ms window instead
of the many short ones a rotating listen needs.

## Decision

Channel policy is decided by what kind of reply a listen is waiting for, not by which chip is
listening. Three named policies, each used by exactly the loops whose reply shape it matches:

| Policy | Behaviour | Used by |
|---|---|---|
| `HOLD_REQUEST_CHANNEL` | Never retunes, never dwells; one `wait_for_packet()` call spans the whole remaining window. | `wait_for_key_challenge_()`, `wait_for_key_confirm_()` (pairing); `wait_for_first_response_()`, `wait_for_final_response_()` (every command exchange) |
| `ROTATE_ALL_CHANNELS` | CH1→CH2→CH3→CH1, hopping after every empty dwell or ignored frame. | `wait_for_discovery_response_()` (pairing discovery) |
| `ROTATE_SKIPPING_REQUEST` | The two channels that are not the request channel; stays put after a reception instead of hopping. | `collect_broadcast_responses()` (the broadcast roll-call) |

All three share one primitive, `ExchangeEngine::listen()`, parameterised by a `ListenSpec`. The
policy is chosen once, at the call site, by the caller stating which kind of reply it is waiting
for — never by asking the driver.

A consequence worth stating plainly: **a holding listen needs no dwell at all.** Slicing exists
so a rotating loop gets a chance to hop between channels and so a long silent wait keeps feeding
the watchdog; `wait_for_packet()` already feeds the watchdog internally while it blocks, so a
`HOLD_REQUEST_CHANNEL` listen has nothing to gain from slicing and every expired slice on the
soft-PHY chips costs a full RX re-arm (standby → clear IRQ → buffer base → packet params → RX).
This is what let the two chip-specific "exchange dwell" constants disappear entirely rather than
being folded into one shared value — there was never a real number to share, because a holding
wait does not need one.

What survives as the one remaining chip-specific wait-timing knob is `RadioDriver::hop_dwell_ms()`
(renamed from `discovery_hop_slice_ms()`): the dwell a *rotating* listen spends per channel before
moving on. That is a chip question — how long must this radio sit on a channel after retuning
before it can hear anything at all — and it now answers it for both rotating listens, discovery
and the roll-call alike, through the same user-facing tuning field each driver already exposed.

The per-chip spread is large (5 ms on SX1276, 200 ms on SX1262 and LR1121) because retuning costs
wildly different amounts on the two families, which is visible in the drivers themselves. The
SX1276 changes channel with three register writes and never leaves RX, so a hop is essentially
free. The SX1262 and LR1121 share `SoftPhyDriverBase`, whose `change_frequency()` must go standby
→ set frequency → clear stale IRQ/DIO latches → re-enter RX; hopping every few milliseconds there
would spend most of the window re-arming the receiver instead of listening with it.

## Consequences

- **Deleted, not merged:** `RadioDriver::exchange_wait_slice_ms()` and both chip-specific
  constants are gone. There is no chip-specific "exchange dwell" to keep in sync anymore, because
  the two loops that used it no longer dwell at all.
- **The broadcast roll-call now asks about hopping instead of borrowing a unicast answer.** Its
  dwell moves from the deleted exchange constant to `hop_dwell_ms()` on every chip: SX1276
  50 ms → **5 ms**, SX1262 90 ms → 200 ms, LR1121 600 ms → 200 ms. That settles the original
  complaint on all three chips rather than just the one that prompted it.
- **A dwell has to clear two independent floors, and only one of them is a chip property.** The
  move above is what exposed this, and it is the most reusable thing in this ADR:
  1. **The retune cost** — how long this radio needs on a channel before it can hear anything at
     all. Genuinely per-chip, for the driver-level reason described above: 5 ms on SX1276, 200 ms
     on the soft-PHY chips.
  2. **One frame's air time** — a reply is roughly 10 ms on air. A dwell shorter than that will
     retune in the middle of a reception unless something stops it from hopping.

  `hop_dwell_ms()` answers the first floor only. SX1262 and LR1121 clear the second one
  incidentally, because 200 ms is far longer than any frame. SX1276's 5 ms does not clear it at
  all — the fast dwell was only ever safe because pairing discovery paired it with a preamble/sync
  guard that extends a dwell in progress rather than cutting a reception off at the channel
  boundary. The roll-call inherited the fast dwell without inheriting the guard, and hardware
  measurement was unambiguous: `scan_paired_devices` found-rate over 25 scans collapsed from an
  83.3% baseline to **12%**. Giving `collect_broadcast_responses()` the same guard restored it to
  **25/25 scans finding every device**, better than the pre-refactor baseline.

  The rule this leaves behind: **a rotating listen may dwell for less than one frame's air time
  only if it lingers on a detected preamble or sync word.** Fast hopping and preamble gating are
  one design, not two independent choices — which is also how the implementations this 5 ms value
  was modelled on do it.
- **The escape hatch stays, unused.** `ListenSpec::dwell_ms` still exists as a per-loop override
  for the day a loop has a *measured* reason to dwell differently from the others on every chip —
  a difference that is per-chip *and* per-loop, not just per-chip. Both rotating call sites leave
  it at 0 today. Deleting the field instead of keeping it unused would push the next loop-specific
  timing difference back into the drivers, which is the exact mistake this ADR records and closes.
- **The explicit cost:** a device that replies to a unicast request on a channel other than the
  one the request went out on would go unheard by every `HOLD_REQUEST_CHANNEL` loop — there is no
  fallback rotation for that case. Nothing in the golden-frame corpus or any field-logged capture
  has ever shown this. If it ever is observed, the fix is a policy change at that call site
  (`HOLD_REQUEST_CHANNEL` → one of the rotating policies with a stated reason), not a new driver
  virtual.
- **The next per-chip wait-timing workaround has nowhere to hide.** Any future driver override
  that answers "how long should I wait" instead of "how fast can I retune" is now recognisable on
  sight as the same mistake `SX1262_EXCHANGE_RESPONSE_WAIT_SLICE_MS` and
  `LR1121_EXCHANGE_RESPONSE_WAIT_SLICE_MS` were — two drivers found it independently once already.
