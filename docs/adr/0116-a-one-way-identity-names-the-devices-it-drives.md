# ADR 0116: A one-way identity names the devices it drives

<!-- doxygen-label: adr0116 -->

**Status:** Accepted · **Recorded:** 2026-11

Builds on [ADR 0080](0080-linked-remote-activity-is-followed-by-an-opt-in-catch-up-read.md) (a linked remote's press
is followed by a status read) and [ADR 0027](0027-controller-identities-replace-node-addressing-for-1w.md) (one-way
controller identities).

## Context

A device that also obeys one of the hub's one-way identities can be commanded over one-way: a STOP sent as the
identity, for example, stops a cover that the identity enrolled on. One-way carries no reply, and the hub does not
hear its own transmission, so the device entity keeps showing its old state after such a command until something else
makes the hub read it.

A real remote listed in the device's `linked_remotes:` already has the right follow-up: its press sets the shown state
at once and schedules a status read (ADR 0080). The hub's own identity cannot be listed there: the build rejects an
identity's address under `linked_remotes:`, because a remote heard on the air and the hub's own transmission are
different things.

## Decision

- **The link lives on the identity.** A `oneway_controllers:` entry may list the devices it drives under `drives:`, by
  their `io_device_id`. One identity can drive several devices. The build rejects an entry that is not a declared
  device, and a device whose kind cannot follow the identity's commands.
- **The hub follows its own transmitted command like a linked remote's press.** When a burst reached the radio and its
  frame carries a command, each driven device gets the same follow-up as after a press of a linked remote: the shown
  state at once, then a status read after the same delay. The follow-up reads the wire values of the frame that went
  out, so an inverted kind or `invert_position:` needs no special case.
- **The read afterwards is the only confirmation.** Nothing confirms that the device obeyed a one-way command; the
  status read corrects the shown state if it did not.
- **A burst that did not reach the radio changes nothing.** Enrollment and un-enrollment frames carry no command and
  change no device state. The hub fires no sender event for its own frames.
- Listing an identity's address under `linked_remotes:` stays rejected; the message points to `drives:`.
- No option reroutes a two-way device's own STOP over one-way: no device on record needs it.

## Consequences

- A cover or light driven through a one-way identity shows the commanded state at once and is corrected by a read,
  as with a real remote.
- A device that ignored the one-way command shows the commanded state until the read arrives.
- A driven device is one of the linked devices that the opt-in catch-up read of ADR 0080 reads, and the hub's own
  command opens that catch-up burst like a linked remote's press.
- A typed command addresses the identity's device class. A driven device of another known class is read afterwards but
  not moved at once, as for a linked remote; with `execute_broadcast: all` every driven device is moved.
- An identity without `drives:` behaves as before.
