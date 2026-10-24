# ADR 0048: 868 MHz io-homecontrol over a Bridle `gfsk_raw` radio

<!-- doxygen-label: adr0048 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

[Bridle ADR 0001](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0001-one-protocol-for-every-band-radios-offer-phy-profiles.md) made the band a PHY
profile and [ADR 0047](0047-home-io-control-drives-a-bridle-co-processor-as-one-more-radio-driver.md)
put it behind `BridlePhy`. The second profile, `gfsk_raw`, carries 868 MHz io-homecontrol on an
SX1262 behind a co-processor ([Bridle ADR 0003](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0003-a-profile-states-what-every-radio-of-it-does.md)). A `gfsk_raw` radio has no framing and no CRC: it sends a
preamble, a sync word and the bytes it is given, and receives exactly `rx_len` bytes behind the
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
- **Fixed-length reception.** The radio captures `rx_len` = 48 bytes behind the sync word
  `57 FD 99`; `unwrap()` finds a CRC-valid frame in them at any bit alignment. Noise is `BAD_CRC`.
- **A wake-up is a long preamble.** The profile needs `per_packet_preamble`, so the hub's start
  preamble, not a strobe, reaches a duty-cycled receiver. The preamble tuning keys
  (`normal_start_preamble` and the like) apply on this band only.
- **The receiver bandwidth is the hub's.** `tuning.sx1262_rx_bandwidth` is sent as `rx_bw_hz`;
  `sx1262_post_tx_settle_us` is not, because the co-processor owns the turnaround and the hub
  warns when it is set.
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
  1024-byte preamble, 2 of 2 at 4096 bytes, where the exchange engine stops after the two tries that
  fit the budget); a one-hour idle run showed no resync, no link timeout, no radio fault and no
  receive overflow.
- **Values the bench set.** The receiver's preamble detector is 16 bits (8 bits reports a false
  preamble about four times a second on a quiet bench), the sync timeout is 250 ms (20 ms loses 22
  of 60 wake-up frames in the idle scan), and the co-processor's clear-channel settle is 113 µs.
- **No real io-homecontrol device has been addressed through the bridge yet.** The SX1262 board
  with the Wio module stays listed as not validated in [Hardware](../hardware.md) until a real device has
  answered.
