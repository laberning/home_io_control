# ADR 0131: An overheard status frame stamps link health and nothing else

<!-- doxygen-label: adr0131 -->

**Status:** Accepted, amends [ADR 0022](0022-unauthenticated-status-frames-are-never-applied.md) · **Recorded:** 2026-11

## Context

[ADR 0022](0022-unauthenticated-status-frames-are-never-applied.md) drops a status frame the hub did not ask for:
nothing proves its sender holds the system key, so its content is never applied. The frame was logged as an issue and
otherwise ignored.

Where a second two-way controller shares a device, such frames are ordinary traffic. A VELUX KLF 200 polls its devices
continuously (corpus `velux_klf200_statuspoll_roundrobin_868_sx1262`,
[issue #98](https://github.com/laberning/home_io_control/issues/98)), and every answer to it reaches the hub as a
status frame addressed to someone else. The hub then showed a device as last seen long ago although it had been on
the air seconds before, and reported normal traffic as a fault.

A device's last contact and signal strength describe the reception of a frame, not what the frame claims. Neither
feeds a decision: the wake-up choice reads only moving evidence and status reports the hub itself authenticated.

## Decision

- **A status frame from a registered device to another controller stamps that device's link health**: last contact
  and signal strength. Position, state and every other reading stay as they are (ADR 0022 is otherwise unchanged).
- **It is logged at INFO at most once per device in ten minutes**, saying that the device answered another controller
  and that the frame was not applied, and at DEBUG otherwise.
- **A status frame from a node that is not registered** is handled as before: it is logged as an ignored frame and
  stamps nothing.
- **A status reply addressed to the hub that arrives outside an exchange** is treated the same way: link health only,
  DEBUG.

## Consequences

- "Last seen" and the signal strength of a device shared with another controller follow the device's real activity.
- A forged frame can make a device's last contact and signal strength look fresh. Neither value was ever proof of
  life, since a replayed capture does the same to any receiver, and no decision reads them.
- The device's state still comes only from the hub's own authenticated reads.
