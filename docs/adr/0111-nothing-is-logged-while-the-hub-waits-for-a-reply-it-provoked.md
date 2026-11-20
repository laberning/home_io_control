# ADR 0111: Nothing is logged while the hub waits for a reply it provoked

<!-- doxygen-label: adr0111 -->

**Status:** Accepted, widens the rule of [ADR 0097](0097-every-preamble-is-set-as-a-duration-the-same-on-every-radio.md) · **Recorded:** 2026-11

Builds on [ADR 0003](0003-shared-software-phy-base-for-sx1262-lr1121.md) (the software-PHY
radios finish a reception early, from the sync word on).

## Context

[ADR 0097](0097-every-preamble-is-set-as-a-duration-the-same-on-every-radio.md) records that a garage drive ignored
every command whose answer to its challenge came late, because logging had slowed the answer
([issue #163](https://github.com/laberning/home_io_control/issues/163)). The rule that followed: nothing is logged
between the challenge and the answer.

The same rule matters on the other side of the answer. A device that is answered starts its reply within a millisecond
and the reply is over after a few. On an LR1121 board the hub noticed the sync word of the 15-byte final reply of a
dimmer 9 to 18 ms after its own transmission ended, measured against an SDR capture of the same exchange. The cause was
log output between the 0x3D answer and the start of the wait for the final reply: the `Auth challenge` line, and in
frame-log builds up to six held frame-log lines. Logging to the API and the UART is synchronous, so the listen loop
began late. No reply was lost, because the radio is re-armed inside `send_packet()` and holds the frame, but anything
the hub measures while a reply is on air (a live signal strength) came too late, an exchange finished later than it
needed to, and a second frame arriving after the chip's fixed-length reception had ended would be missed.

The cause sits in the shared exchange engine, so every radio has it; what it spoils differs by chip.

## Decision

- **A quiet window per provoked reply.** From a request's transmit to the end of the first-response wait, and from the
  0x3D's transmit to the end of the final-response wait, the exchange engine emits no log line.
- **The frame log is held across the window.** The hold starts before the request's transmit and ends with the last wait
  of the try (the first wait for an exchange without a challenge, the final wait after one). A frame the hub rejects
  flushes the held lines at once, because the hub keeps listening and the lines describe the past. The hold buffer takes
  12 lines, enough for the request, the challenge, the 0x3D and the final reply with a margin for a probe line.
- **Ordinary log lines move out of the window.** The `Auth challenge` line is printed after the final wait; its values
  are kept in the exchange context. There is no general buffering of ordinary log output: only the frame log keeps a
  hold buffer.
- **Every step that transmits and then waits for the reply it provoked follows the same rule.** That covers the
  exchange engine's two waits and every other such step: the roll-call, the inbound challenge, and the pairing engine's
  discovery, discover-confirm, key-init (held on across the 0x32 that answers its challenge and the wait for its
  confirm, with that wait's tries), key-confirm (including its challenge answer) and key-pull waits. Each holds the
  frame log from before its transmit to the end of its wait, so the driver's own re-arm line after the transmit is held
  too. A new step that waits for a provoked reply does the same; one that does not is a defect, not a reason to widen
  the rule.
- **The hold nests, and a scope type states it.** The hold keeps a depth counter and only the outermost release prints
  the held lines. `FrameLogHoldScope` (`log_frame.h`) holds from its declaration to its `release()` or to the end of the
  scope, so an early return cannot leave the frame log held. The exchange engine releases explicitly, because its window
  spans several functions; it releases on every path out of a try.
- **A frame that does not end the wait flushes the held lines.** `listen()` prints them when the handler ignores a
  frame, because the hub keeps waiting and the lines describe the past. A long wait that hears many frames, such as a
  roll-call or a discovery, therefore never overflows the hold buffer.
- **Ordinary log lines of a step follow its wait.** The pairing key-confirm line that reports a challenge and the
  key-init's `Challenge (0x3C) received` line are printed after the confirm wait, like the `Auth challenge` line.
- **The hold depth is tracked in every build.** Only the buffering of lines needs `IOHOME_FRAME_LOG`; the counter does
  not, so the host tests check at every transmit and every hand-over of a reply that the frame log is held. The counter
  also counts releases, so a test can check that no release, which prints the held lines in a frame-log build, falls
  between a challenge and the answer to it.

## Consequences

- The hub notices a reply while it is still on air, on every board and with the frame log on or off.
- The lines of an exchange appear after its wait, with their content and order unchanged; a live log shows them a few
  milliseconds later than before.
- A frame-log build keeps about 2 KB more RAM for the larger hold buffer.
- What goes on air and the retry ladders are unchanged. In a frame-log build the 0x3D can follow the challenge a little
  sooner, because the hub starts its wait on time and notices the challenge earlier.
- A new log line inside a transmit-then-wait step is checked against this rule: the exchange tests fail if a line is
  logged between a transmit and the hand-over of the reply, and the tests of every step that holds fail if the frame
  log is not held at a transmit or at a hand-over; the exchange and key-confirm tests also fail if it is released
  between a challenge and the 0x3D, and the key-init tests if it is released between a challenge and the 0x32.
- The order of two lines of a pairing step can differ from before: a line a wait logs when it times out (a "no response"
  note) now precedes the held frame-log lines of that step, the key-confirm challenge line follows the second wait, and the key-init's
  challenge line follows the key transfer's confirm wait.
