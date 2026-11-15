# ADR 0086: The idle receiver keeps the loop running so its channel scan follows the protocol dwell

<!-- doxygen-label: adr0086 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0044](0044-chip-neutral-transmit-intent-wake-level-idle-scanning-and-air-time.md) (the idle
scanning point: the host still hops, and is now asked often enough to keep the dwell) and
[ADR 0070](0070-a-preamble-on-air-keeps-the-soft-phy-receiver-running.md) (its preamble hold now also
keeps the idle hop on the channel, on every chip, and is renewed there).

## Context

While the hub is idle, it listens for one-way remotes and for reports a device pushes on its own, and
moves the receiver across the three hop channels. The hop is timed by `HOP_TIME_US` (2.7 ms per
channel, the public reference value), but it is checked only once per pass of the ESPHome loop, and
the loop runs every 16 to 30 ms. So the receiver stays on one channel for a whole loop interval, and
sits on the channel a remote uses only about a third of the time. A reporter's count (issue #230)
fits that: a remote's transmissions whose copies all carry a short start preamble were missed in 3 of
20 presses at rest and 7 of 20 while another shutter travelled, with the log quiet at every missed
press; transmissions with a long wake-up preamble were always heard.

Holding the idle hop on a preamble raised two further problems on the bench:

- **A hold that ends at a fixed time loses long preambles.** A wake-up preamble (about 218 ms on air)
  is far longer than a 30 ms hold. After each capped hold the hop left the channel for about 8 ms
  and came back. A preamble of fixed length puts its sync word at the same phase of that
  hold-and-away cycle in every copy, and on the LR1121 that phase fell at the end of the hold or in
  the gap, so most long copies were lost (about 43 % heard against 71 % without the hold). A longer
  cap fixed the LR1121 but cost the SX1262 (the 250 ms variant heard fewer copies), and a cap
  matched to the preamble would park the scan that long on every false detection. Restarting the
  receiver on the same channel when the hold ends, and holding on if the preamble is detected again
  (about 1 ms later on both soft-PHY chips), heard 73-79 of 80 long copies on all three boards
  tried, and kept the short-copy rate.
- **The SX1276's 250 ms hold parks the receiver on the wrong channel.** It holds from the first
  PreambleDetect sighting for the longest start preamble. With the hop checked every 2.7 ms, a
  false or adjacent-channel detection (a strong transmitter on one channel seen on the next) kept
  the receiver on the wrong channel for 250 ms. In 50 short bursts it did so 8 times and lost 3 to 4
  copies in 7 of the bursts.

## Decision

1. **The hub asks ESPHome for a continuous loop while its radio is set up and not failed**, through
   ESPHome's high-frequency loop request, and releases it when setup fails or the radio is marked
   failed. One rule decides it. The idle hop then runs on its own timer, so the receiver visits each
   channel for about `HOP_TIME_US`. A frame arriving still holds the channel (issue #81), and so
   does a start preamble whose sync word is still to come: the dwell is shorter than a start
   preamble, so a receiver that lands inside one has to stay for the frame. Exchanges, discovery and
   the key-extraction listen keep their own blocking waits.
2. **The preamble hold on the idle hop is renewable, on every chip.** A hold lasts one short slice
   from the first sighting of a preamble (30 ms: a dwell plus the longest measured preamble-to-sync
   of a short start frame). When the slice ends, the receiver restarts on the same channel instead
   of letting the hop leave, and keeps holding if the preamble is detected again within 4 ms. No
   renew starts once the run of holds on one channel is 300 ms old, which covers a wake-up preamble.
   With no preamble detected again the hop goes on. The rule is one chip-neutral type that every
   driver uses; the SX1276 gives up its 250 ms hold for the same short slice. The blocking listens
   keep ADR 0070's plain hold, which ends at its cap.
3. **The idle dwell stays `HOP_TIME_US` for every chip.** A chip gets its own idle dwell, through a
   driver contract, only if a bench comparison shows it clearly catches fewer copies at 2.7 ms.
4. **No option to turn it off.**

## Consequences

- One-way remote presses with short-preamble copies are heard while the hub is idle, as vendor
  receivers hear them.
- The ESP32's loop task no longer sleeps between passes: more CPU time and a little more power on a
  board that is mains-powered.
- A false preamble detection on noise holds the idle receiver on one channel for one slice (30 ms) and
  then for 4 ms more at most, on every chip. Only a detector that fires again after every restart
  can hold it longer: up to the 300 ms bound plus the last slice, about a third of a second.
- A long wake-up preamble keeps the receiver on its channel to the sync word instead of leaving and
  returning during it.
- A renew restarts the receiver, so a frame whose sync word arrives within about a millisecond of
  the restart is lost. A renew happens only when a preamble has been on air for a whole slice, which
  no short frame's preamble is.
- A radio that scans the hop channels by itself (ADR 0044's `run_idle_scan()`) is unaffected: the
  host does not hop while it scans.
