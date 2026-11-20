# ADR 0112: A frame's RSSI is its own or unknown

<!-- doxygen-label: adr0112 -->

**Status:** Accepted · **Recorded:** 2026-11

Builds on [ADR 0003](0003-shared-software-phy-base-for-sx1262-lr1121.md) (the software-PHY radios finish a reception
early, from the sync word on) and [ADR 0111](0111-nothing-is-logged-while-the-hub-waits-for-a-reply-it-provoked.md)
(the hub notices a reply promptly after its own transmission).

## Context

Every received frame carries a signal strength that the hub shows and logs: the RSSI sensor and link health, roll-call
and scan output, pairing telemetry and advice, one-way sightings and the exchange debug line. It is only worth
showing when it belongs to the frame it is attached to.

On the LR1121 it often did not. The driver read the chip's packet status, which the chip updates at its own end of
packet. The early-completion path takes the frame from the buffer before that, so the packet status still described the
previous reception: in a bench run 72 of 89 consecutive reads were byte-identical, and a device at -48 dBm showed
-97 dBm after a foreign or noise reception. The packet status after RX_DONE is the frame's own. Turning early reception
off would fix the value but delays the answer to a challenge by about 5 ms, which turnaround-sensitive devices do not
tolerate ([ADR 0097](0097-every-preamble-is-set-as-a-duration-the-same-on-every-radio.md)).

A live level read right after the sync word is noticed is the frame's own while the frame is on air, and the noise floor
once it has ended. The SX1276 has the same shape: its signal register is live, and a frame whose sync no poll saw in
flight can only be read at the end of the frame, where it is the noise floor. The SX1262 latches its packet status at
sync and is not affected.

## Decision

- **A capture reports the frame's own level, or none.** `RadioCaptureInfo` carries `rssi_known` (default true);
  `rssi_dbm` is meaningful only when it is set. `rssi_or_unknown()` gives the level as `RSSI_UNKNOWN_DBM` when it is not.
  A reading that belongs to another reception or to the channel's floor is never reported as the frame's level.
- **Two chip-neutral hooks on the early path.** `SoftPhyDriverBase` calls `sample_level_at_sync()` as the first action
  after the sync word is noticed and `sample_level_after_frame()` once the early path holds a frame received inside a
  wait. Both do nothing by default. The SX1262 overrides neither; the RxDone path keeps the packet status on every chip.
- **The LR1121 checks its live read against the floor inside a wait.** For a frame received inside a wait (the reply to
  the hub's own transmission) it reads the instantaneous level in both hooks and reports the level at sync only when it
  stands at least `LR1121_LATE_NOTICE_MARGIN_DB` (6 dB) above the level after the frame; otherwise the sync was noticed
  after the frame ended, and the frame has no level. The test needs no protocol timing constant; a frame within 6 dB of
  the floor is not decodable anyway. A frame completed outside a wait takes no floor and reports the level at sync:
  the next copy of a one-way burst can already be on air after the frame, which would make the floor read equal the
  frame's own level and drop a correct reading.
- **The SX1276 reports unknown** for a frame whose sync no poll saw while it was on air, instead of the level at the
  end of the frame.
- **Consumers treat unknown as no sample.** Link health leaves the last value and the average unchanged; roll-call,
  scan, pairing telemetry, one-way sightings, overheard frames and the exchange debug line print `n/a`; the pairing
  advice leaves the level out of its text.

## Consequences

- A shown or logged signal strength on any radio is the frame's own, within a few dB of a reading taken through the
  chip's normal end of packet, or `n/a`.
- The early path pays a short SPI read on an LR1121 before the buffer reads, and inside a wait a second one after the
  frame. The challenge to 0x3D turnaround is checked on air.
- A reply noticed after its end (a loaded loop pass) gives no sample instead of a wrong one; an idle frame noticed
  late is not detected; a rare gap in the signal sensor replaces a misleading value.
- The 6 dB margin is a chip constant, not user-tunable.

## Considered options

- **Turn early reception off on the LR1121.** Rejected: the answer to a challenge is about 5 ms later.
- **Check the floor on the idle path too.** Rejected: one-way bursts make the floor read equal the frame's own level,
  so correct readings would be dropped.
- **Detect a stale packet status.** Rejected: once the early path stops using the packet status there is nothing stale
  to detect, and a live read needs only the floor to be judged.
- **Time the late notice from the hub's own transmission.** Rejected: it covers replies only, needs a timing
  constant, and has no reference on the idle path.
