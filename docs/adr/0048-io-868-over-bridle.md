# ADR 0048: 868 MHz io-homecontrol over a Bridle `gfsk_raw` radio

<!-- doxygen-label: adr0048 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

[Bridle ADR 0001](https://github.com/laberning/bridle/blob/bridle-v0.9.0/docs/adr/0001-one-protocol-for-every-band-radios-offer-phy-profiles.md) made the band a PHY
profile and [ADR 0047](0047-home-io-control-drives-a-bridle-co-processor-as-one-more-radio-driver.md)
put it behind `BridlePhy`. The second profile, `gfsk_raw`, carries 868 MHz io-homecontrol on an
SX1262 behind a co-processor ([Bridle ADR 0003](https://github.com/laberning/bridle/blob/bridle-v0.9.0/docs/adr/0003-a-profile-states-what-every-radio-of-it-does.md)). A `gfsk_raw` radio has no framing and no CRC: it sends a
preamble, a sync word and the bytes it is given, and receives exactly `rx_capture_bytes` bytes behind the
sync word. The io frame coding is therefore the host's to do.

## Options considered

- **Code the frame in the co-processor.** The firmware would then know io-homecontrol, and every
  coding fix would need a re-flash of the radio.
- **A second `RadioDriver` for the 868 bridge.** Duplicates the supervised link, the single receive
  path and the transmit sequence.
- **`Io868GfskPhy` beside `Io2g4Phy`** (chosen). The driver stays band-agnostic.

## Decision

- **The coding stays on the host.** `Io868GfskPhy` appends the io CRC and UART-codes the frame with
  the same routines the software PHY of the SX1262 and LR1121 drivers uses, so a frame is on air
  what theirs is. The co-processor sees only bytes.
- **Fixed-length reception.** The radio captures `rx_capture_bytes` = 48 bytes behind the sync word
  `57 FD 99`; `unwrap()` finds a CRC-valid frame in them at any bit alignment. Noise is `BAD_CRC`.
- **The band is the 868 MHz band.** `Io868GfskPhy` returns the hub's default band (`IO_868_BAND`,
  [ADR 0132](0132-a-radio-describes-its-band-and-the-hubs-rules-read-it.md)): channel 2 for wake-up requests,
  the 868 MHz windows and gap, not low-power by default.
- **A wake-up is a long preamble.** The profile needs `per_packet_preamble`, so the hub's start
  preamble, not a strobe, reaches a duty-cycled receiver. Every preamble is a duration
  ([ADR 0097](0097-every-preamble-is-set-as-a-duration-the-same-on-every-radio.md)); the profile turns it into
  programmed bytes at the radio's 8 bit periods per byte. The radio must accept the longest wake-up the
  hub is configured with: a configured `low_power_wake_ms` that does not fit the co-processor's
  `max_preamble_bytes` fails setup with a log line naming both numbers, and a change at run time that does not fit
  is refused with a warning; a wake-up is never shortened silently.
- **The receiver bandwidth is the hub's.** `tuning.sx1262_rx_bandwidth` is sent as `rx_bw_hz`, and
  `sx1262_discovery_hop_slice_ms` is the idle hop's dwell. No other chip's key is read, and the co-processor owns
  the turnaround after a transmission, so the post-TX settles are not.
- **A key the band's radio ignores is rejected on that band.** Which keys each band reads follows from what its
  profile reads (the table is `_APPLIES_TO` in `tuning.py`). Setting another key in YAML fails validation with
  the key and the band in the message, and no entity is created for it. The table speaks of the radio only: a key
  that depends on a feature the hub offers, such as the key-extraction responder's, is read whatever the band and
  stays accepted.
  - *868 MHz bridge* rejects `io2g4_hop_dwell_ms`, `sx1262_post_tx_settle_us`, `lr1121_post_tx_settle_us`, the
    SX1276 and LR1121 bandwidth and discovery-slice keys, and the response-preamble byte keys of every chip. It
    reads the preamble durations, as it sends the long preamble.
  - *2.4 GHz bridge* reads no chip key: `Io2g4Phy` sends no tuned param, and its dwell is `io2g4_hop_dwell_ms`. A
    preamble duration reaches it only as a wake level, and its train is fixed, so it rejects `start_preamble_ms`,
    `cold_broadcast_reply_preamble_ms`, `response_preamble_ms` and `low_power_wake_ms` (and their deprecated byte
    keys) as well. `pairing_discovery_preamble_ms` stays: at its ceiling of 300 ms it selects the long train. Its
    deprecated byte key goes: at most 1024 bytes, about 213 ms, it never does.
  - The three SPI radios keep every key but `io2g4_hop_dwell_ms`, so a chip's key on another chip's board stays
    accepted as before.
- **Selected by `bridle_band: io_868`.** `set_bridle_band` picks the phy; a co-processor that
  cannot do the profile or the 868.25 to 869.85 MHz band is rejected at `ready`.

## Consequences

- A hub reaches either band with the same firmware sources; the choice is a config key.
- The 868 behaviour is checked against the same exchange suites and the same recorded corpus as the
  other radio drivers, and against a simulated co-processor in the full-stack tests, before any air time.
- **Measured against a reference radio and phantom nodes.** On a bench with a Heltec V3 running this
  component as the reference radio and a XIAO nRF52840 with a Wio-SX1262 behind an ESP32-C3 host: the
  co-processor's transmissions decode on the reference radio, byte for byte, on all three channels
  (20 of 20 each); the co-processor receives the reference radio's frames (49 to 50 of 50 on one
  channel, 89 of 90 through the idle scan with a 1024-byte wake-up preamble, 51 of 51 through the host);
  the host's exchanges to phantom devices put every try on the air (3 of 3 at the 48-byte and the
  1024-byte preamble, 2 of 2 at 4096 bytes, where that run's exchange engine sent two tries; a low-power
  target now gets three, [ADR 0095](0095-a-resting-battery-device-gets-a-2048-byte-wake-up-and-three-attempts.md));
  a one-hour idle run showed no resync, no link timeout, no radio fault and no receive overflow.
- **Values the bench set.** The receiver's preamble detector is 16 bits (8 bits reports a false
  preamble about four times a second on a quiet bench), the sync timeout is 250 ms (20 ms loses 22
  of 60 wake-up frames in the idle scan). The clear-channel settle after a retune is the co-processor's own:
  it holds its reply to a `get_rssi` until the receiver listens on the new frequency (specification §3.1), so the
  hub adds none ([ADR 0134](0134-a-radio-may-have-no-reading-and-an-attempt-without-one-fails.md)); the reply bound
  of a `gfsk_raw` radio is 1100 µs. A preamble held for the 250 ms sync timeout would keep an unforced retune
  waiting, so the channel check's retune is forced ([ADR 0134](0134-a-radio-may-have-no-reading-and-an-attempt-without-one-fails.md)).
- **No real io-homecontrol device has been addressed through the bridge yet.** The SX1262 board
  with the Wio module stays listed as not validated in [Hardware](../hardware.md) until a real device has
  answered.
