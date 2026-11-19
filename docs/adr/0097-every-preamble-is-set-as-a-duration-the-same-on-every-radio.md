# ADR 0097: Every preamble is set as a duration, the same on every radio

<!-- doxygen-label: adr0097 -->

**Status:** Accepted, amends [ADR 0042](0042-start-preamble-default-comes-from-the-radio-driver.md) and [ADR 0095](0095-a-resting-battery-device-gets-a-2048-byte-wake-up-and-three-attempts.md) · **Recorded:** 2026-11

## Context

Every preamble the hub sends is set in bytes: the wake-up for a sleeping battery device
(`low_power_wake_preamble`), the start of a request (`normal_start_preamble`, with a default per
radio, [ADR 0042](0042-start-preamble-default-comes-from-the-radio-driver.md)), the answer inside a
session (one key per radio: `sx1262_response_preamble`, `sx1276_response_preamble`,
`lr1121_response_preamble`), the reply to a cold broadcast (`cold_broadcast_reply_preamble`) and the
discovery (`pairing_discovery_preamble`, following the start preamble,
[ADR 0078](0078-discovery-follows-the-start-preamble.md)).

Measured with an SDR on our bench, the same number of bytes does not last the same time on every
radio: the SX1276 sends each preamble byte as 10 bits, the SX1262 and the LR1121 as 8. 2048 bytes are
533 ms on an SX1276 and 427 ms on an SX1262; the 32-byte start preamble of the SX1276 is about 8.7 ms
and the 48-byte one of the SX1262 about 10.1 ms; a 12-byte response preamble is about 3.4 ms on the
SX1276 and 2.6 ms on the SX1262. The per-radio defaults of ADR 0042 and the per-radio response keys
existed to make up for that difference without naming it. A receiver hears time, not bytes.

Evidence for the durations:

- **Wake-up:** a VELUX KLI 110 remote sends about 600 ms of wake-up, and with about 600 ms all eight
  commands to two resting VELUX SSL shutters landed on the first try
  ([PR #240](https://github.com/laberning/home_io_control/pull/240)); a VELUX remote kept the channel
  busy 0.45 to 0.6 s before its first frame
  ([issue #209](https://github.com/laberning/home_io_control/issues/209)); at 427 ms a resting SSL
  answered 12 of 16 tries ([issue #151](https://github.com/laberning/home_io_control/issues/151)).
- **Start:** about 10 ms from an SX1262 reached a Somfy Izymo dimmer on every first try (ADR 0042;
  the LR1121 shares that radio's software PHY); the SX1276's 8.7 ms did too.
- **Response:** on the SX1262, raising the response preamble from 8 to 12 bytes (2.6 ms) stopped most
  lost closing replies ([issue #119](https://github.com/laberning/home_io_control/issues/119)); the
  SX1276 already sends about 3.4 ms with its 12 bytes. The answer must not start much later, though:
  a motor that advertises a 5 ms turnaround lost every answer that listen-before-talk held back by
  5 ms ([issue #178](https://github.com/laberning/home_io_control/issues/178)), and a garage drive
  whose answers logging already slowed ignored every command answered behind a 6.7 ms response
  preamble on an SX1262 ([issue #163](https://github.com/laberning/home_io_control/issues/163)). 3.5 ms starts
  the answer about 1 ms later than 2.6 ms.

## Decision

- **Every preamble setting is a duration in milliseconds**, the same on every radio. Each driver turns
  a duration into its own number of preamble bytes from the bits it sends per byte; no code outside
  the drivers names a radio for this.
- **Defaults:** wake-up 600 ms, start 10 ms, answer inside a session 3.5 ms, reply to a cold broadcast
  20 ms; the discovery follows the start unless set.
- **One setting per kind:** the three per-radio response keys become one. The byte keys are still
  accepted for a while, with a warning, and are converted to the duration they give today on that
  radio; setting a byte key and its duration key together is refused.
- One-way bursts keep their own wake-up preamble (1024 bytes); their other copies follow the start
  preamble, as before.

## Consequences

- A configuration means the same on every board, and moving a hub to another board keeps its timing.
- On an SX1262 or LR1121 board the wake-up grows from 427 to 600 ms and the answer preamble from 2.6
  to 3.5 ms; on an SX1276 board the wake-up grows from 533 to 600 ms and the start preamble from 8.7
  to 10 ms. Each change is checked on the bench before the commit.
- An exchange to a resting battery device that does not answer blocks the loop for about 3.5 s
  (ADR 0095: 3.3 s), up to about 4.9 s when it challenges every attempt and never closes the exchange
  (ADR 0095: 4.6 s); a roll-call of both power classes puts about 1.9 s on air (ADR 0095: 1.3 s).
- A 600 ms wake-up uses about 17 % of the hour that the EU duty cycle allows on the request channel;
  the hub does not enforce that limit today.
- ADR 0029 (the start preamble follows the target's power class), ADR 0040 (a low-power device's
  start preamble follows its wake belief) and ADR 0078 (the discovery follows the start) still hold,
  in milliseconds.

## Considered options

- **Keep bytes and per-radio defaults.** Rejected: the same value means different times, and every
  new radio needs its own set of defaults.
- **Keep each radio's present duration as its default.** Rejected: the point is one duration for a
  receiver, whatever board sends.
- **A shorter wake-up such as 500 ms.** Not chosen yet: untested; the #240 contributor is asked to
  try 500 to 600 ms, and a shorter default can follow from that.
