# ADR 0026: 1W enrollment is required, and its safety comes from a physical interlock
<!-- doxygen-label: adr0026 -->

**Status:** Accepted · **Recorded:** 2026-08

## Context

A 1W device only acts on commands from source addresses it has itself registered as a
controller, via its own association-mode procedure — not merely from anyone holding the right
key. That makes enrollment (registering the hub as a controller) the precondition for any 1W
command this project sends ever moving anything, including devices the hub already drives over
2W. Since the hub can register itself as a controller on hardware it does not own, the question
this ADR settles is: what makes that safe to ship with no confirmation step in front of it?

## Options considered

1. **An arming switch**, following ADR 0021's bootloader-rewrite precedent: a hub-level entity
   that must be turned on before the enroll button will transmit, visible in Home Assistant so
   "am I about to do this" is answerable by looking.
2. **A software confirmation window** (press once to arm, press again within N seconds to
   commit).
3. **No software gate at all** — rely on constraints the protocol and this feature's own design
   already provide.

## Decision

Option 3. There is no arming switch and no confirmation window. Two independent properties make
this safe, not one:

**1. Only a person standing at the receiver can ever open the window a `0x30` needs.**
Enrollment is two-sided, and only one side is something the hub can ever emit:

| Half | Who does it | What it is |
|---|---|---|
| Receiver enters association mode | **a person, physically** | a multi-second hold on the actuator's own PROG button, confirmed by the actuator's own indicator |
| Controller offers its credential | **the hub** | one short `0x39` burst, immediately followed by one short `0x30` burst — the documented pairing handshake, see "What the enroll button sends" below |

A device that is not in association mode does not accept a `0x30` at all. So a hub sitting on
someone's shelf, or one whose YAML happens to carry `enrollment: true` for longer than intended,
cannot enroll into anything: no device is listening. This interlock cannot be triggered from
software or from range — someone has to be standing at the actuator with their finger on it.

**2. Even a successful enrollment only adds a controller; it does not remove *another* one.** A
device that already has a registered remote keeps obeying that remote after the hub also enrolls —
enrollment is additive across controllers, not a takeover. This bounds the *consequence* of the
first property ever failing (an enrollment nobody meant to trigger still leaves every existing
controller working), independently of whatever bounds who can trigger it in the first place.
Neither property depends on the other: the interlock would still be worth having even if
enrollment were destructive, and additive registration would still limit the blast radius even if
the interlock were somehow bypassed.

Both properties fall out of how the protocol and this feature are already built — neither had to
be added for safety's sake.

ADR 0021's arming switch exists for a different risk shape: an *irreversible* bootloader write,
where one more entity is a fair price for the write's own sake. Borrowing that pattern here would
add friction without adding safety, since the thing that actually makes a stray enrollment
attempt harmless — nobody holding the PROG button, and nothing being displaced even if someone
is — is already present with or without a switch.

**What the build flag (`enrollment: true`) is for instead:** not safety against a stray press,
but against the capability sitting in a permanently-running build. It is the same "presence is
the whole gate, add-and-remove-and-reflash is the lifecycle" shape as `accept_foreign_pairing`
and `recover_oneway_key`, and the docs say so: remove the line once you are done enrolling.

**What the enroll button sends (revised 2026-08-21, hardware-confirmed 2026-08-21).** The
documented 1W pairing handshake (`reference/iown-homecontrol/docs/linklayer.md:396`, "1W
Discovery") is `0x39` immediately followed by `0x30`, both from the same controller, back to back
within one gesture — and this project's own
`tests/corpus/captures/enrollment/somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml` shows a real
Somfy Smoove remote doing exactly that against a real Izymo, 128 ms apart, same burst. This ADR
originally decided the enroll button would send `0x30` alone and keep `0x39` behind its own
explicitly-named action, on the strength of a single 2026-08-13 bench success with `0x30` alone.

A 2026-08-21 retest against that same Izymo appeared to reproduce a failure with `0x30` alone,
initially read as evidence of a stale controller-table slot needing `0x39` to clear. **That
reading was wrong.** The retest's `oneway_controllers` config had been copied from an external bug
report and carried `io_device_type: roller_shutter`, not `light` — 1W is class-addressed typed
broadcast, so the frames never reached the Izymo at all, regardless of which handshake was used.
Once the device class was corrected, **both forms worked**: `0x30` alone enrolled successfully,
and so did `0x39` then `0x30`. The stale-controller-table-slot hypothesis is therefore neither
confirmed nor needed to explain anything observed so far — it remains a plausible failure mode for
some other receiver family, just not a demonstrated one.

The enroll button sends `0x39` then `0x30` anyway — not because `0x30` alone is known to fail on
this hardware, but because that is what a real remote's own pairing gesture does on the wire (the
Smoove capture above), and matching the documented, hardware-observed handshake is the safer
default for the many 1W receiver families this project has not bench-tested. `0x30`-alone remains
a known-working fallback if some other receiver ever turns out to react badly to the `0x39`
prelude — nothing observed rules that out either, with two data points.

**This does not weaken property 2 above.** The `0x39` this button sends carries only this
identity's own `src` address — nothing on the wire lets a `0x39` frame name a *different*
controller for removal. So this prelude can only ever clear this identity's own prior entry
before re-registering it; it still cannot touch another remote's registration. "Enrollment is
additive across controllers, not a takeover" continues to hold; what changed is only that
enrollment is no longer required to be non-destructive *to a previous attempt by this same
identity*, which was never a safety property this ADR relied on.

**Standalone removal stays behind its own action.** `oneway_remove_controller` (the native API
action that fires `0x39` alone, with no following `0x30`) remains separate — for un-enrolling
without immediately re-enrolling. What changed is only that "Enroll" no longer sends `0x30` in
isolation; the self-directed `0x39` it now sends first was never the removal this section's
original wording was guarding against.

This decision does not depend on standalone removal working. Safety here rests on enrollment being
hard to trigger and low-consequence to *other* controllers when it happens, not on being able to
undo it afterward.

## Consequences

- **The risk is bounded to hardware the operator can physically reach, and further
  bounded by what a stray enrollment can actually do.** A hub cannot enroll into a neighbour's
  actuator over the air — someone has to walk up to that actuator and hold its button first — and
  even if that happened unintentionally, the device's existing controller keeps working
  alongside the new one. Both limits are independent of each other and of this feature's own
  code.
- **No second entity, no confirmation state to reason about.** The enroll button's only guard is
  the YAML build flag; whether a press does anything at all is decided entirely by the receiver.
- **Removal (`oneway_remove_controller`, 0x39) is not confirmed working on this project's test
  hardware.** Repeated attempts had no observable effect. The leading hypothesis, by analogy with
  enrollment's own physical gate, is that `0x39` also needs the receiver in its association-mode
  window and the failed attempts were fired without it — but this is untested, not ruled out.
  Treat "enrollment is reversible" as design intent, not a demonstrated fact, until this is
  retested and confirmed.
- **Both enrollment failures traced to real users in the same week (2026-08-21: an external
  tester's Velux KUX 110 report, and this project's own Izymo retest) turned out to share one
  root cause: a device-class mismatch in `oneway_controllers`' `io_device_type`, not the
  enrollment handshake.** Once the class was corrected, the Izymo enrolled with either `0x30`
  alone or `0x39` then `0x30`. Sending `0x39` first is not what fixed this class of failure and is
  not required to; it is kept because it matches a real remote's own captured on-wire behavior
  (`tests/corpus/captures/enrollment/somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml`). A different,
  still-live failure mode is a timing mismatch between the receiver's PROG hold and the
  controller's press — a long hold where a short press was needed, or vice versa — which the docs
  state both halves and their asymmetry explicitly for.
- **If a future revision ever discovers association mode can be triggered remotely, or held open
  indefinitely, on some device family, the first property above weakens and the arming-switch
  question should be revisited.** Nothing observed so far suggests that; the additive-registration
  property would still hold regardless.
