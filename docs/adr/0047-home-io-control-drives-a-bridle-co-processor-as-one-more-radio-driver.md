# ADR 0047: Home IO Control drives a Bridle co-processor as one more radio driver

<!-- doxygen-label: adr0047 -->

**Status:** Accepted, amends [ADR 0023](0023-reference-material-as-a-corpus-origin.md) (a fifth corpus origin) · **Recorded:** 2026-09

## Context

[Bridle ADR 0001](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0001-one-protocol-for-every-band-radios-offer-phy-profiles.md) chose a radio
co-processor behind a UART, spoken to with the [Bridle protocol](https://github.com/laberning/bridle/blob/bridle-v0.6.0/README.md). The hub side has to plug into
code that was written for chips on an SPI bus: the exchange engine, the pairing flow and the
entity platforms talk to the radio only through `RadioDriver`, and the hub is an `spi::SPIDevice`
whose schema always asked for an SPI bus.

Four things about a bridge differ from an SPI radio and decide the shape of the driver:

- The band is not the chip's. The same bridge can carry 2.4 GHz io-homecontrol today and
  868 MHz later, so the framing, the wake-up (a strobe, not a long preamble) and the band's values
  (channels, reply windows, busy limit, low-power default) belong to a band, not to the driver.
  [ADR 0132](0132-a-radio-describes-its-band-and-the-hubs-rules-read-it.md) holds how a radio hands
  the band to the hub.
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
  payload and back, how a wake-up level becomes a preamble or a strobe, the idle scan, and the band's
  description (`band()`, [ADR 0132](0132-a-radio-describes-its-band-and-the-hubs-rules-read-it.md)).
  `Io2g4Phy` is the 2.4 GHz profile. The codec (`radio_bridle_codec.h/.cpp`) is pure and reused by the
  test co-processor; the serial link is a `ByteStreamAccess` the way SPI is a `SpiAccess`, implemented
  over ESPHome's UART by `UartByteStream`, the only file that includes its headers.
- **One receive path.** One private drain reads the stream, deframes it and routes each message; a
  reply goes to the command awaiting it, a frame into a small FIFO, everything else into state.
  Every getter and wait loop drains first. When our own `tx_done` is routed, the FIFO is cleared:
  what arrived before it arrived before or during the transmission ([specification §6](https://github.com/laberning/bridle/blob/bridle-v0.6.0/README.md#6-ordering-rule)).
- **The band's values are the band's.** The 2.4 GHz band is `IO_2G4_BAND`: the channels at 2425, 2450
  and 2475 MHz, every channel a wake-up channel, short reply windows and gap, low-power by default and a
  busy limit of -75 dBm while no quiet level is known. The limit is the most permissive level IEEE
  802.15.4's energy-detect rule allows (10 dB above the -85 dBm receiver sensitivity), which is also
  Nordic's default. The hub reads them through `RadioDriver::band()` and tests no band; the tuning keys
  that exist on every radio (`lbt_rssi_threshold_dbm`, `exchange_start_response_wait_ms`) override them.
- **The band has its own tunable, and a key the band's radio ignores is rejected on that band.** `io2g4_hop_dwell_ms`
  sits next to its `TuningConfig` field and is read through `Io2g4Phy`. A tuning key names the radios it applies to,
  where a radio is a radio type and, for a bridge, its band, in one place that code generation and the entity list
  both read. A key the radio ignores is rejected in YAML with a message naming the key and the band, and has no
  entity there: the 2.4 GHz hop dwell applies to the 2.4 GHz bridge only, and the bridge on 2.4 GHz ignores the other
  chips' keys and the preamble durations (a wake-up there is a fixed train, [ADR 0048](0048-io-868-over-bridle.md)
  lists the keys of each band). The three SPI radios keep every key whatever their chip. A future native 2.4 GHz
  driver reuses the key.
- **Wi-Fi sits in the band.** A bridge on 2.4 GHz shares the band with the hub's Wi-Fi. The rule is
  [ADR 0132](0132-a-radio-describes-its-band-and-the-hubs-rules-read-it.md)'s: the hub gives the engine
  the centre frequency of its Wi-Fi channel, and the channels of the band within 15 MHz of it are left out of
  a request. The hub logs one INFO line after Wi-Fi connects when that takes a channel out, naming the io channel
  it gives up and the Wi-Fi channels that would keep it; on 868 MHz nothing is logged.
- **Link supervision never blocks.** `init()` blocks once, in `setup()`. After that a lost link (no
  answer to the 30 s heartbeat, a missing `tx_done`, three event-counter gaps in ten seconds, an
  unsolicited `ready`) starts a resync that advances one step per poll from the calls the hub
  already makes on every idle pass. Reset pulses are at least five seconds apart. Three failed
  resyncs within a minute put the link *down*, not failed: sending fails at once, and one attempt is
  made every minute, so a co-processor that was unplugged or re-flashed returns without rebooting
  the ESP32. Only an `init()` failure latches the driver failed.
- **Compiled only when configured.** Every Bridle source sits under `IOHOME_BRIDLE`, which
  `to_code()` defines for `radio_type: bridle`; the hub's `setup()` is the one place that names the
  driver. The codes come from `radio_bridle_enums.h`, the generated
  header of a Bridle release, committed at the top of the component directory (ESPHome copies only the
  files directly in a component's directory) and replaced only by `scripts/bridle-update.sh`, which
  checks it against the release's `SHA256SUMS`; `BRIDLE_VERSION` names the release and `make bridle-check`
  recomputes the hashes. The header is in the checkout, in an `external_components` clone and in CI, with
  no generation step. An SX1262 build never includes it, and its image does not grow. `make clang-tidy`
  analyses every source that holds bridge code in a build that configures a bridge.
- **The trace goes through the held log.** With `bridle_trace:` the driver logs every message, and while the hub
  waits for a reply it provoked nothing is printed ([ADR 0111](0111-nothing-is-logged-while-the-hub-waits-for-a-reply-it-provoked.md)):
  the lines wait in the frame log's hold and follow when the wait ends. The hold takes one authenticated exchange
  whose request the co-processor refuses once for a busy channel: 36 lines (10 for each of two channel checks: the
  retune and four readings, each with its reply; 4 for the refusal: the `tx`, its `error`, the co-processor's `log`
  event and the warning that prints it; 8 for the exchange; 4 spare), and 12 more of the frame log's own in a build
  with `IOHOME_FRAME_LOG`. `to_code()` sets `IOHOME_HELD_LOG_LINES` to 36 or 48 as a build flag, and the driver's
  source checks the number at compile time, exactly. At 320 bytes a held line, that is 11,520 or 15,360 bytes of RAM
  in every bridge build; an SPI build with the frame log keeps 12 lines. A second busy channel can need more lines than
  the hold has, and a hold that overflows prints how many lines it lost.
- **Unawaited commands.** Every Bridle command gets one reply. `RadioBridle` keeps a small table of
  commands nobody waits for (`rx_on`, `rx_scan`, `idle`), matches each `ack` or `error` against its head,
  resyncs the link on a lost or overtaking reply, and does not retry a refused `rx_scan` or `rx_on` for a
  few seconds. `BridlePhy::radio_fits()` decides in one place whether a co-processor's radio can carry a
  band ([Bridle ADR 0002](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0002-every-command-gets-exactly-one-reply.md),
  [ADR 0003](https://github.com/laberning/bridle/blob/bridle-v0.6.0/docs/adr/0003-a-profile-states-what-every-radio-of-it-does.md)).
- **`tx_power` is per radio.** The SPI radios keep the range 0…22 and the default 17. A bridge takes
  any dBm value the protocol allows and, when it is unset, asks its radio for its own maximum; the
  co-processor clamps and echoes what it applied.
- **A bridge is configured by UART, band and trace.** `uart_id` names the UART (1 Mbaud, or a lower rate the
  co-processor is built for; the 2.4 GHz band needs 460800 or more), `bridle_band` the band (`io_2g4` by default,
  `io_868`) and `bridle_trace` the trace. The chip keys of an SPI radio (`cs_pin`, `busy_pin`, the DIO pins,
  `tcxo_voltage`) are rejected, `spi:` is not needed, and the 2.4 GHz band takes no one-way controllers and no
  one-way key recovery: no one-way frame has been seen on 2.4 GHz.
- **Tests without RF.** A scripted co-processor (`FakeCoprocessor`) plays the spec's scenario event
  streams, injects faults and plays recorded sessions; the exchange engine runs over it in the same
  cases as over `MockRadio`; the 2.4 GHz captures replay through it; the fixtures in
  `tests/corpus/sessions/` are recorded from the driver's own tests and replayed against it.

## Consequences

- The hub, the exchange engine and the platforms need nothing of the bridge but what any radio gives
  them: `RadioDriver::band()`, the radio's own tuning check (`accepts_tuning()`) and the byte-stream seam of the
  driver. The hub's composition adds one branch to the radio choice and skips the SPI registration when there
  is no SPI bus.
- A 2.4 GHz frame's log line shows its air frequency, as every radio's does ([ADR 0132](0132-a-radio-describes-its-band-and-the-hubs-rules-read-it.md)).
  `RadioBridle` writes the frame log's TX and RX lines through `log_frame()`, the call the SPI drivers make, so
  `-DIOHOME_FRAME_LOG` shows a bridge's frames like any radio's and the frame-log tips of the docs apply unchanged. A
  TX line follows the co-processor's `tx_done` (a `tx` it refuses for a busy channel logs none), an RX line follows
  each frame delivered to the hub (a frame with a bad CRC logs none), and both go through the held log.
- The reset pulse of a resync is a 20 ms block in the loop, the only one; a supervised link costs
  no other `loop()` pass more than a few milliseconds.
- Recordings made from the driver's own tests pin its command sequence and prove nothing about a
  real co-processor; they carry origin `synthetic-bootstrap` and are replaced by real recordings as
  those exist.
- A user of `radio_type: bridle` needs nothing beyond the component directory: no Python step, no Java,
  no specification files.
- **The hub's challenge answer (0x3D) carries the low-power flag; the 2.4 GHz capture of a vendor controller
  shows it clear.** The hub marks every 0x3D it sends with `CTRL1_LOW_POWER` (`0E 20 ...` in its own 868 MHz
  captures, which every device there accepts). The VELUX KLR 200 on 2.4 GHz answers with CTRL1 `0x00`
  (`velux_klr200_exchange_execute_open_2g4`), as a VELUX KIG 300 does on 868 MHz
  (`somfy_rs100_statuspoll_kig300_sx1276`). The exchange replay of the 2.4 GHz capture expects the hub's frame
  to differ from the captured one in that flag alone, the HMAC included. Whether a 2.4 GHz device accepts the
  flag set is an open hardware question: the capture shows what one controller sends, not what a device
  accepts, so only a bench run that answers a 2.4 GHz device's challenge from the hub can settle it.
- **2.4 GHz captures have an origin of their own.** The captures of 2.4 GHz traffic come from contributors who
  sniffed their own installation and sent the log privately with permission to publish it. Neither
  `github-issue` (no public thread holds the log) nor `reference-material` (no public source whose licence could
  be checked) describes that truthfully, so [ADR 0023](0023-reference-material-as-a-corpus-origin.md)'s list
  gains a fifth value, `contributor`: a real installation's traffic captured by someone other than the
  maintainer. `issue` is `null` unless a public thread exists, `source.device` names the devices and the
  capturing tool, frames are literal as captured, and key material is re-keyed before the first commit exactly
  as for `reference-material`. The CRC and FCS checks at ingest and the crypto checks after re-keying are what
  stand behind such a capture's bytes.
- **On 2.4 GHz overheard status frames are normal traffic.** Every VELUX remote there is two-way, so each remote
  press and each gateway poll draws a 0x04 or 0x71 the hub did not ask for. [ADR 0022](0022-unauthenticated-status-frames-are-never-applied.md)
  holds unchanged: the content is never applied and the hub's own poll, which the remote's request schedules,
  sets the state. The frame stamps only link health (last contact and RSSI), which describe its reception, not
  what it claims, and it is logged at INFO once per device per ten minutes and at DEBUG otherwise.
