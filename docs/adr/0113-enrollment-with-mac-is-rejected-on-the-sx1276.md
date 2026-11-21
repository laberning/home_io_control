# ADR 0113: `enrollment_with_mac` is rejected on the SX1276

<!-- doxygen-label: adr0113 -->

**Status:** Superseded by [ADR 0124](0124-enrollment-with-mac-is-removed.md), amends [ADR 0032](0032-oneway-velux-enrollment-gesture.md) · **Recorded:** 2026-11

Narrows [ADR 0032](0032-oneway-velux-enrollment-gesture.md) (one-way enrollment gestures, which honour
`enrollment_with_mac`) for one radio.

## Context

`enrollment_with_mac: true` makes a one-way identity send its add-controller frame (`0x30`) with a six-byte MAC trailer
after the frame's declared length: a 35-byte frame instead of 29. The SX1262 and the LR1121 send the frame as a raw byte
stream they frame in software, so the trailer goes out like any other byte.

The SX1276 sends through its packet engine in io-homecontrol mode (`IoHomeOn` in `RegPacketConfig2`). The engine takes
the packet length from the frame's own first byte and appends the CRC itself. The datasheet documents the mode only as
the enable bits; it describes no way to send further bytes after the engine's CRC in the same packet.

A bench capture on a Heltec V2 shows the result. Without the trailer each copy of the add-controller frame is one burst
of about 276 ms (267 ms preamble and the frame), and the hub reports it sent. With the trailer each copy is one carrier
of about 533 ms: the declared frame decodes with a valid CRC after the preamble, then the carrier stays on with the six
bytes still in the FIFO (FifoLevel set, FifoEmpty clear), PacketSent never comes, and the driver's transmit-done guard
ends the copy: `TX timeout after 528 ms — DIO0 never fired` for each of the four copies, then
`only 0 of 4 copies transmitted`. Earlier measurements of 270 to 530 ms extra per copy (issue #74 and our own bench) were
the same effect before that guard existed.

No real remote in the corpus sends the trailer, and no device is known to need it.

## Decision

- A configuration with `enrollment_with_mac: true` on an identity and `radio_type: sx1276` fails validation, with a
  message that says the SX1276 cannot send the trailer and that the option needs an SX1262 or LR1121 board.
- The option stays available on the SX1262 and the LR1121.
- The transmit-done guard is not lengthened to hide the symptom: a longer wait would only keep the carrier on longer.

## Consequences

- An SX1276 user can no longer build an enrollment that silently reports "not sent" on every copy; the former build
  warning for this case is replaced by the error.
- Nothing changes on the air for any configuration that builds.
- No device is known to accept the trailer: the one enrollment sent with it to a real device, a Somfy Izymo dimmer from
  an SX1276 board, delivered the declared frame without it.
- If a device turns out to need the trailer, an SX1276 board would need a transmit path outside the packet engine (raw
  mode with software framing), which is a driver change of its own.

## Considered options

- **Program the packet length explicitly** (fixed-length mode with the full 35 bytes). Rejected: in io-homecontrol mode
  the length comes from the frame's first byte, and the documented registers give no way to make the engine send bytes
  after its own CRC.
- **Raw transmit for this one frame on the SX1276.** Rejected for now: a second transmit path in the driver for an
  option no known device needs.
- **Keep the warning.** Rejected: the bench shows the enrollment is never sent, so a warning understates it.
