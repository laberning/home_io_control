# ADR 0117: A one-way controller can show an assumed-state cover

<!-- doxygen-label: adr0117 -->

**Status:** Accepted · **Recorded:** 2026-11

Builds on [ADR 0116](0116-a-one-way-identity-names-the-devices-it-drives.md) (the hub follows its own one-way
command) and [ADR 0080](0080-linked-remote-activity-is-followed-by-an-opt-in-catch-up-read.md) (a listed remote's press
moves a two-way device's shown state). Stays within
[ADR 0018](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md) (the hub persists nothing but its counters).

## Context

A one-way-only cover (a motor that only listens) has no device address the hub can read. Today a one-way identity
gives Home Assistant buttons only: open, close and stop, with no state. Users who want a cover in Home Assistant build a
template cover with a timer and an automation that sends the wall remote's command a second time when the remote is
pressed (issue #74). The doubled transmission costs airtime, and the template knows nothing the hub knows.

The hub already knows two things that move such a cover: its own one-way commands, reported after each burst with the
intent of the frame that went out (ADR 0116), and the presses of real remotes it hears, after its duplicate filter.

## Decision

- **An option of the one-way identity, not a device.** A `oneway_controllers:` entry of a cover kind may ask for a
  cover entity (`cover:` with a name). The entity is a companion of the identity, like its buttons; it has no device
  address, takes no part in the cover platform's schema and is never read.
- **Assumed state.** The entity is marked as assumed state in Home Assistant. Its state follows a pure rule: an open
  command or press shows open, close shows closed, stop shows stopped with the position unknown, a position shows its
  nearer end. There is no travel time and no moving phase.
- **Two inputs.** The identity's own commands, after a burst that reached the radio; a command that did not reach the
  radio changes nothing. And the presses of the remotes listed under `follows:` on the same entry, by sender address,
  after the duplicate filter; nothing is sent for them. A remote that is not listed never moves the state, so a
  neighbour's remote of the same class cannot.
- **Open, close and stop on the entity** queue the same one-way commands as the identity's buttons.
- **Remote presses are wire values;** the entity maps them through the identity's effective inversion, so "open" means
  open for the user. The entity reaches the hub through two port calls: one subscribes to the de-duplicated presses of
  given senders, the other asks for the identity's effective inversion.
- **Nothing is persisted** (ADR 0018). An ESPHome cover has no unknown state, so after a boot the entity shows open
  until the first command or press. Home Assistant's cover without a position has no stopped state either: after a
  stop the entity shows open and idle, as the two-way cover does for a discrete cover that is not fully closed.
- Only cover kinds, decided from the kind's capabilities. An entry without `cover:` generates exactly what it did
  before.

## Consequences

- A one-way-only cover appears in Home Assistant without a template and without sending the remote's command twice.
- The shown state is wrong when the hub missed a press, when the device ignored a command (out of range, a desynced
  counter, a sensor's lock) or when the motor was stopped by hand; the docs say so.
- After a boot, and after a stop, the entity shows open: an assumption like every other state it shows.
- A position from travel time is left to a later change.
