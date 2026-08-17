# ADR 0026: 1W enrollment is required, and its safety comes from a physical interlock

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
| Controller offers its credential | **the hub** | one short `0x30` burst |

A device that is not in association mode does not accept a `0x30` at all. So a hub sitting on
someone's shelf, or one whose YAML happens to carry `enrollment: true` for longer than intended,
cannot enroll into anything: no device is listening. This interlock cannot be triggered from
software or from range — someone has to be standing at the actuator with their finger on it.

**2. Even a successful enrollment only adds a controller; it does not remove one.** A device that
already has a registered remote keeps obeying that remote after the hub also enrolls — enrollment
is additive, not a takeover. This bounds the *consequence* of the first property ever failing (an
enrollment nobody meant to trigger still leaves every existing controller working), independently
of whatever bounds who can trigger it in the first place. Neither property depends on the other:
the interlock would still be worth having even if enrollment were destructive, and additive
registration would still limit the blast radius even if the interlock were somehow bypassed.

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

**Removal stays behind its own action.** `0x39` (remove-controller) is reachable only through the
explicitly-named `oneway_remove_controller` native API action, never as an automatic prelude to a
press of "Enroll." Hiding a removal inside a button labelled "Enroll" would be the one way this
feature could still surprise a user despite everything above.

This decision does not depend on removal working. Safety here rests on enrollment being hard to
trigger and low-consequence when it happens, not on being able to undo it afterward.

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
- **The one recorded real-world failure of the enrollment procedure was a timing mismatch
  between its two halves** — a long hold where a short press was needed, or vice versa — not a
  missing safeguard. The docs state both halves and their asymmetry explicitly for this reason.
- **If a future revision ever discovers association mode can be triggered remotely, or held open
  indefinitely, on some device family, the first property above weakens and the arming-switch
  question should be revisited.** Nothing observed so far suggests that; the additive-registration
  property would still hold regardless.
