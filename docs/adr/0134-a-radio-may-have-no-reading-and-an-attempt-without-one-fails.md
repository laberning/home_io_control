# ADR 0134: A radio may have no reading, and an attempt without one fails

<!-- doxygen-label: adr0134 -->

**Status:** Accepted, amends [ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md) · **Recorded:** 2026-11

## Context

[ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md) judges a
channel by readings: up to five before a request, one reading at or above the busy limit makes the channel
busy, and one reading at every tenth idle hop teaches the hub the board's quiet level. It takes for granted
that every reading is a level. The three SPI radios always have one: the chip's register holds it.

A co-processor behind a [Bridle](https://github.com/laberning/bridle/blob/bridle-v0.9.0/README.md) link may have
none. The Bridle 0.8 specification gives a `get_rssi` one meaning on every profile: a fresh reading of the
frequency the receiver listens on, taken after the command arrives, with the receiver listening (§3.1). No level
stands for "unknown" any more. When the co-processor cannot read, it answers `error(radio_busy)` (its own
transmission or its turn back to receive, or a retune waiting behind a frame in progress, §2.2) or
`error(radio_fault)`, or the reply does not come. The reply bound is stated per profile, in µs from the command's last
byte in to the reply's first byte queued: 650 for 802.15.4 and 1100 for `gfsk_raw` (§4). Link transit and the
host's own processing are outside it. A reading taken while the co-processor scans cannot be attributed to a
channel by the host (§3.1).

The hub needs a way to say "no reading" and a rule for what an attempt does with it. Reading a missing level as
quiet would let a request go out onto a channel nobody measured, which is what a fixed level of -127 dBm made
it do before. Reading it as busy would keep the radio off the air for a fault that says nothing about the air.

## Decision

- **A reading is optional.** `RadioDriver::read_rssi()` and `RadioDriver::measure_channel_level()` return
  `std::optional<int16_t>`: a level in dBm, or nothing when the radio could not take one. The SPI radios
  always return a level. A bridge returns nothing when its link is down, while it scans, when the
  co-processor answers `radio_busy` or any other `error`, and when no reply comes within the wait for it.
- **An attempt without a reading fails.** When a reading of the channel check is missing, the engine ends the
  attempt as `NO_READING`: nothing is sent, the attempt counts in `no_reading_attempts` and in neither
  `channel_busy_attempts` nor the busy checks, no back-off is spent, and silence after it is no evidence about
  the receiver (the same as an attempt that ended busy). The plan's next attempt follows. The rule is the
  engine's, once, for every radio.
- **A `radio_busy` answer is a reading the hub did not get.** The attempt fails; the hub does not ask again within the
  attempt. The driver counts it (`radio_busy`, `no reply` and `refused` in the debug dump), so how often it happens
  can be read from a running hub.
- **Quiet-level learning skips a missing reading.** A fault of the co-processor never moves the learned limit.
- **A forced send ranks only the channels that have a reading** (`lbt_force_when_busy`). When none of the
  channels the request may use has one, the attempt fails instead of picking a channel by guessing.
- **A reading is of the channel it is asked for.** A bridge cannot tell from its recorded frequency where its
  receiver is (`RadioDriver::reading_needs_retune()`): a scan hides its channel, and an unforced retune to that
  very frequency may still wait behind a frame. Every channel check therefore starts with a retune, and
  `read_rssi()` on a bridge that scans gives nothing. An idle bridge is told to receive before a reading, so the
  first check after a reset reads the air instead of failing on `not_receiving`.
- **A reading is used only when the retune before it was accepted.** A co-processor that refuses a retune stays on
  the old channel and the `get_rssi` right behind it reports that channel's level, so a bridge's `read_rssi()`, the
  one place every caller reads through, gives nothing while its newest receive command stands refused, forced or not.
- **A bridge retunes for a channel check with a forced `rx_on`.** The retune of a channel check
  (`RadioDriver::retune_for_reading()`, which `measure_channel_level()` calls; the default is
  `change_frequency()`, because the SPI radios retune at once) sets `force` (Bridle 0.8, §3.1). A frame in
  progress on another channel, a preamble held for `sync_timeout_us` included, is then dropped as
  `rx_abort(forced_retune)` and the reading follows at once, as on the SPI radios. The retune is sent also when
  the recorded frequency already is the channel to check: by the specification a later `rx_on` replaces a retune
  that waits (§2.2), and a forced `rx_on` to the frequency the radio is on changes nothing (§3.1), so a frame
  arriving on that channel is not dropped. The forced retune is sent once per channel check, before its first
  reading, and again after any hop, scan, `idle` or send in between: until then it is the driver's newest command,
  and nothing can wait behind it. A check that follows another on the same channel with nothing sent in between (a
  back-off on a request that may use one channel only) needs none of its own for the same reason. On an idle bridge
  the forced `rx_on` also starts the receiver. The abort is not a fault: the driver ends its frame-in-progress
  state and nothing else. Every other retune waits for the frame (the idle hop,
  a rotating listen, the key-extraction session channel, a locked channel, the `rx_on` of `set_mode_rx()`,
  a `tx`, and an `rx_scan`, which has no `force`), so a frame the hub wants is never cut off by a retune it sent
  before it saw the frame.
- **The bridge profiles carry no settle time.** `cca_settle_us()` of the bridge is 0: the co-processor holds
  the reply until it has read, so no figure that belongs to one co-processor firmware stays in the hub. The SPI
  radios keep their chip's settle.
- **The wait for a reply is link supervision: the command reply timeout.** A `get_rssi` waits as long as every
  other command does (`ACK_TIMEOUT_MS`, 500 ms), because that exceeds what a reading can take by two orders of
  magnitude: the co-processor answers within its reading deadline (1100 µs on `gfsk_raw`, 650 µs on 802.15.4, the
  `GET_RSSI_REPLY_US` of the specification's profile tables, §4), the command and its reply take 16 bytes on the wire
  (160 µs at 1 Mbaud), and the hub's own latency is measured in the hub's clock. On 868 MHz, over 5059 readings, the
  hub-side share is at most 4.5 ms with the 1100 µs bound and 4.7 ms with the co-processor's measured maximum of
  859 µs, and the round trip at most 5.7 ms. On 2.4 GHz, over 2086 readings, the hub-side share is at most 4.1 ms
  with the 650 µs bound and 4.3 ms with the measured maximum of 508 µs, and the round trip at most 4.9 ms. An
  earlier 868 MHz run measured 33.5 ms at the first check after boot, delayed by the hub's own log lines at boot;
  that is about a fifteenth of the wait. A reading without a reply is therefore not a slow reading but a command or
  reply the link lost, and it takes the path of every command without a reply: the attempt that asked has no reading
  (`no reply` in the debug dump) and the link resyncs (§7), as after a `tx` without its `tx_done`. There is no second
  timeout and no separate handling of a late `rssi`; once the link resyncs, a reply that still arrives is an
  unmatched reply like any other. A `radio_fault` that answers the reading arrives within the wait and is handled by
  the branch below.
- **A refused command has changed nothing** (§2.2), `channel_busy` and `radio_fault` excepted. After a refused
  `tx` the driver puts its own record of the radio's state back and sends no `rx_on` to make sure, which it did
  before to cover a state it could not tell.
- **A `radio_fault` resets the link wherever it comes from**, unsolicited or as the answer to any command and
  also after the wait for that command ended (§7). One branch of the driver's reply routing decides it.

## Consequences

- A co-processor that cannot read costs requests their attempts. It never causes a send into a channel nobody
  measured or a false "channel busy".
- A `radio_busy` answer to a reading is possible only during or right after the bridge's own transmission (or its
  turn back to receive): the forced retune before a channel check's first reading, sent again after any hop,
  scan, `idle` or send in between, leaves no retune waiting behind a frame, a preamble held for `sync_timeout_us`
  included, whatever frequency the driver had recorded. A check on a channel the receiver is already on costs one
  `rx_on` (12 bytes) and its `ack` (7 bytes), about 190 µs on the wire at 1 Mbaud, once per check; the co-processor
  does nothing for it. The engine asks for a reading after a retune and after listening, not directly after its own
  transmission, so such an answer is rare; the counter shows how often it happens.
- **The readings of a check are further apart over the bridge.** [ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md)
  takes up to five readings about 1 ms apart, which is the SPI radios' spacing. Over the bridge a reading is a
  round trip, so the next one follows the link's round trip plus the pause between readings. Measured in the hub's
  clock, from one request to the next: on 868 MHz about 2 ms (median 2.2, minimum 1.8, 99th percentile 5.0, maximum
  7.7 ms), a whole check about 9 ms and at most 17 ms (median 9.2, maximum 16.5 ms); on 2.4 GHz (802.15.4) about
  2 ms (median 2.0, minimum 1.8, 99th percentile 5.0, maximum 6.4 ms), a whole check about 9 ms and at most 15 ms
  (median 9.1, maximum 14.5 ms). The engine's pacing does not change.
- The SPI radios' generated code and 868 MHz behaviour do not change.

## Considered options

- **Treat a missing reading as quiet** (the former level of -127 dBm). Rejected: a request would go out onto a
  channel nobody measured.
- **Treat a missing reading as busy.** Rejected: a fault of the co-processor would keep the radio off the air and
  say that the air is busy.
- **Retune a channel check without force, as every other retune.** Rejected: a preamble held for
  `sync_timeout_us` keeps the retune waiting, the reading is refused as `radio_busy` and the attempt is lost, where the
  SPI radios would have read the new channel at once.
- **Ask again within the attempt after `radio_busy`.** Not decided here; it can be added without changing the rule for
  an attempt without a reading.
