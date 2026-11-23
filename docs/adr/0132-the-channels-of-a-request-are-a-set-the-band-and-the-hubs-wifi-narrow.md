# ADR 0132: The channels of a request are a set the band and the hub's Wi-Fi narrow

<!-- doxygen-label: adr0132 -->

**Status:** Accepted, amends [ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md) · **Recorded:** 2026-11

## Context

[ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md) gives every request a
free channel: a request draws a channel other than the previous attempt's, and a request to a low-power receiver
stays on channel 2. The exchange engine holds that second rule as a test of the low-power flag in four places, and
the limit it uses for "busy" before the board's quiet level is known is one constant.

Both are facts of the 868 MHz band, not of the protocol or of the hub. The hub layer knows no chip and should know no
band either: a radio that carries the same protocol on another band has its three channels elsewhere on the air, may
wake its receivers on any of them, and has another receiver sensitivity. Written as hub rules, each such difference
would become a branch on the radio.

A band in the 2.4 GHz range is shared with the hub's own Wi-Fi. A Wi-Fi channel is 20 MHz wide, so one of the hub's
three channels can lie inside it, where the hub's own Wi-Fi traffic covers a request however free the channel
measured a moment before.

## Decision

- **A radio describes its band as data.** `RadioDriver::band()` returns a plain value: the air frequency of each of
  the three channels, the channels a wake-up request may use, and the busy limit used while no quiet level is known.
  The default is the 868 MHz band with the values of ADR 0096 (channel 2 for wake-up requests, -85 dBm).
- **The channels a request may use are one set**, computed by one pure function: all three; for a request to a
  low-power receiver the band's wake-up channels; less every channel whose air frequency lies within 15 MHz of a
  frequency the hub asks to keep away from. If that leaves no channel, the exclusion is dropped.
- **The draw and the forced send take that set.** A request draws a channel of the set other than the previous
  attempt's; `lbt_force_when_busy` sends on the quietest channel of the set. The engine no longer tests the low-power
  flag to choose a channel.
- **The frequency to keep away from comes from the hub**, which knows its Wi-Fi channel; the band decides what it
  means, by air frequency. On 868 MHz no channel is ever inside the guard.
- **The 15 MHz guard** is half a 20 MHz Wi-Fi channel (11 MHz with the transmit mask's skirt) plus a margin. It is
  this project's choice, not a measured value.
- **No channel is preferred.** The hub keeps no memory of the channel that last carried an answer.

## Consequences

- On 868 MHz nothing changes on the air: the set of a low-power request is channel 2, every other request draws from
  all three, and the fallback limit is -85 dBm.
- A radio on another band needs no rule in the hub: it returns its band.
- A hub whose Wi-Fi sits on one of the band's channels gives that channel up for requests. With Wi-Fi wider than
  20 MHz the guard is too narrow; the hub sees only its primary channel.
- A request that fails on one channel is retried on another, never on the one that worked last; where one channel is
  much better than the others, the first attempt reaches it one time in the size of the set.
