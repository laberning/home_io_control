# ADR 0047: Home IO Control drives a Bridle co-processor as one more radio driver

<!-- doxygen-label: adr0047 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

[Bridle ADR 0001](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0001-one-protocol-for-every-band-radios-offer-phy-profiles.md) chose a radio
co-processor behind a UART, spoken to with the [Bridle protocol](https://github.com/laberning/bridle/blob/bridle-v0.6.0/README.md). The hub side has to plug into
code that was written for chips on an SPI bus: the exchange engine, the pairing flow and the
entity platforms talk to the radio only through `RadioDriver`, and the hub is an `spi::SPIDevice`
whose schema always asked for an SPI bus.

Four things about a bridge differ from an SPI radio and decide the shape of the driver:

- The band is not the chip's. The same bridge can carry 2.4 GHz io-homecontrol today and
  868 MHz later, so the framing, the wake-up (a strobe, not a long preamble) and the clear-channel
  limit belong to a band, not to the driver.
- Everything the radio does comes back as events on one stream, in an order the driver must keep:
  a reply to an earlier try must never be taken for the current one.
- The link can fail at run time in ways an SPI bus cannot (a cable pulled, a co-processor
  re-flashed), and the hub's loop must not stall for seconds when it does.
- The Bridle sources need the protocol's code tables. ESPHome compiles every source file of a component,
  so an SX1262 user would otherwise carry them too.

## Options considered

- **`RadioBridle` as one more `RadioDriver`, with the band behind a `BridlePhy` interface**
  (chosen). The hub, the engine and the platforms are unchanged.
- **A 2.4 GHz driver with the band written into it.** Smaller, but an 868 MHz profile over the same
  bridge would need `if (band)` branches inside the driver.
- **Compile the Bridle sources into every firmware.** Simple, but every image grows.
- **Generate the enum header in every build.** Needs the specification package and Python inside the
  ESPHome build.
- **Commit the generated enum header of a Bridle release** (chosen): no build step, and the header is
  replaced only by a script that checks it against the release.
- **Block in `send_packet()` until a lost link is restored.** Simple, but a co-processor that has
  gone for good would stall the loop for seconds on every call.

## Decision

- **The driver and its profile.** `RadioBridle` (`radio_bridle.h/.cpp`) is a `RadioDriver`. It
  knows the protocol and nothing of the band; a stateless `BridlePhy` (`radio_bridle_phy.h`), injected
  by reference, supplies the PHY profile and its params, how an io frame is wrapped into a radio
  payload and back, how a wake-up level becomes a preamble or a strobe, the idle scan, and the
  band's defaults. `Io2g4Phy` is the 2.4 GHz profile. The codec (`radio_bridle_codec.h/.cpp`) is pure
  and reused by the test co-processor; the serial link is a `ByteStreamAccess` the way SPI is a
  `SpiAccess`, implemented over ESPHome's UART by `HubUartStream`, the only file that includes its
  headers.
- **One receive path.** One private drain reads the stream, deframes it and routes each message; a
  reply goes to the command awaiting it, a frame into a small FIFO, everything else into state.
  Every getter and wait loop drains first. When our own `tx_done` is routed, the FIFO is cleared:
  what arrived before it arrived before or during the transmission ([specification §6](https://github.com/laberning/bridle/blob/bridle-v0.6.0/README.md#6-ordering-rule)).
- **The clear-channel limit is the driver's to name.** `RadioDriver::lbt_threshold_dbm()` returns the
  hub's tunable by default; a band with another limit overrides it. The exchange engine asks the
  driver. The 2.4 GHz default is −75 dBm: the most permissive level IEEE 802.15.4's energy-detect
  rule allows (10 dB above the −85 dBm receiver sensitivity), which is also Nordic's default.
- **The band has its own tunables.** `io2g4_hop_dwell_ms` and `io2g4_lbt_rssi_threshold_dbm` sit
  next to their `TuningConfig` fields and are read through `Io2g4Phy`. A future native 2.4 GHz
  driver reuses them; an 868 MHz profile over a bridge keeps the hub's existing keys.
- **Link supervision never blocks.** `init()` blocks once, in `setup()`. After that a lost link (no
  answer to the 30 s heartbeat, a missing `tx_done`, three event-counter gaps in ten seconds, an
  unsolicited `ready`) starts a resync that advances one step per poll from the calls the hub
  already makes on every idle pass. Reset pulses are at least five seconds apart. Three failed
  resyncs within a minute put the link *down*, not failed: sending fails at once, and one attempt is
  made every minute, so a co-processor that was unplugged or re-flashed returns without rebooting
  the ESP32. Only an `init()` failure latches the driver failed.
- **Compiled only when configured.** Every Bridle source sits under `IOHOME_BRIDLE`, which
  `to_code()` defines for `radio_type: bridle`. The codes come from `radio_bridle_enums.h`, the generated
  header of a Bridle release, committed at the top of the component directory (ESPHome copies only the
  files directly in a component's directory) and replaced only by `scripts/bridle-update.sh`, which
  checks it against the release's `SHA256SUMS`; `BRIDLE_VERSION` names the release and `make bridle-check`
  recomputes the hashes. The header is in the checkout, in an `external_components` clone and in CI, with
  no generation step. An SX1262 build never includes it, and its image does not grow.
- **Unawaited commands.** Every Bridle command gets one reply. `RadioBridle` keeps a small table of
  commands nobody waits for (`rx_on`, `rx_scan`, `idle`), matches each `ack` or `error` against its head,
  resyncs the link on a lost or overtaking reply, and does not retry a refused `rx_scan` or `rx_on` for a
  few seconds. `BridlePhy::radio_fits()` decides in one place whether a co-processor's radio can carry a
  band ([Bridle ADR 0002](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0002-every-command-gets-exactly-one-reply.md),
  [ADR 0003](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0003-a-profile-states-what-every-radio-of-it-does.md)).
- **`tx_power` is per radio.** The SPI radios keep the range 0…22 and the default 17. A bridge takes
  any dBm value the protocol allows and, when it is unset, asks its radio for its own maximum; the
  co-processor clamps and echoes what it applied.
- **Tests without RF.** A scripted co-processor (`FakeCoprocessor`) plays the spec's scenario event
  streams, injects faults and plays recorded sessions; the exchange engine runs over it in the same
  cases as over `MockRadio`; the 2.4 GHz captures replay through it; the fixtures in
  `tests/corpus/sessions/` are recorded from the driver's own tests and replayed against it.

## Consequences

- The hub, the exchange engine and the platforms did not change for a bridge; the only shared-code
  edit is the `lbt_threshold_dbm()` virtual, whose default keeps every SPI radio as it was.
- A 2.4 GHz frame's log line shows the hub's 868 MHz channel numbers, because the logical channels
  are what the hub hops between; the air frequency is the profile's business.
- The hub sends its `0x3D` challenge answer with CTRL1 LOW_POWER set, as the Somfy 868 MHz
  captures show; the VELUX KLR 200 captured on 2.4 GHz sends it clear. The exchange replay asserts
  exactly that difference, so the first 2.4 GHz field result can decide which one the receiver
  needs.
- The reset pulse of a resync is a 20 ms block in the loop, the only one; a supervised link costs
  no other `loop()` pass more than a few milliseconds.
- Recordings made from the driver's own tests pin its command sequence and prove nothing about a
  real co-processor; they carry origin `synthetic-bootstrap` and are replaced by real recordings as
  those exist.
- A user of `radio_type: bridle` needs nothing beyond the component directory: no Python step, no Java,
  no specification files.
