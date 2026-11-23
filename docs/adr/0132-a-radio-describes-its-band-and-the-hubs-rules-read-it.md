# ADR 0132: A radio describes its band and the hub's rules read it

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

The timing of an exchange is held the same way, as constants measured on 868 MHz: how long an attempt listens for the
reply to a start frame, how long it listens for a low-power receiver, and how long the hub pauses between attempts.
The attempt plans of [ADR 0073](0073-a-read-plans-its-attempts-and-an-attempt-must-fit-the-budget-with-its-windows.md)
and [ADR 0095](0095-a-resting-battery-device-gets-a-2048-byte-wake-up-and-three-attempts.md) read them, the pairing
loops read the pause, and a device that declares no `low_power:` counts as mains-powered. A band whose devices answer
within a few milliseconds wastes most of a 400 ms window, and a band whose receivers all sleep needs the low-power
class as its default.

A band in the 2.4 GHz range is shared with the hub's own Wi-Fi. A Wi-Fi channel is 20 MHz wide, so one of the hub's
three channels can lie inside it, where the hub's own Wi-Fi traffic covers a request however free the channel
measured a moment before.

## Decision

- **A radio describes its band as data.** `RadioDriver::band()` returns a plain value. It holds the air frequency of
  each of the three channels, the channels a wake-up request may use and the busy limit used while no quiet level is
  known. It also holds the reply window of a start frame, the reply window of a low-power receiver, the pause between
  attempts, whether a device is low-power unless its configuration says otherwise, and the time after a challenge
  beyond which an answer is reported as late (none on a band without such a limit).
- **The default is the 868 MHz band** with the values in use before: channel 2 for wake-up requests and -85 dBm
  (ADR 0096); 400 ms, 550 ms and 250 ms; not low-power; no late-answer limit.
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
- **The attempt plans keep their shape** and read windows and pause from the band. The short lead attempt of ADR 0095
  listens its own 100 ms or the band's low-power window, whichever is shorter.
- **`exchange_start_response_wait_ms` is unset by default** and, when set, replaces the band's start window and
  nothing else. While it is unset its tuning entity shows no value, as the busy limit's does.
- **`low_power:` has three states**: declared true, declared false, not declared. A device that declares nothing
  takes the band's default when it is registered; a discovery that reports a low-power device still sets it.
- **Log lines show the air frequency** of a channel, through the band. In code a channel stays identified by its
  868 MHz value.

## Consequences

- On 868 MHz nothing changes on the air: the set of a low-power request is channel 2, every other request draws from
  all three, the fallback limit is -85 dBm, and windows and pause keep their values. The "Exchange Start Response
  Wait" entity shows no value until the user sets one; the boot log names the window in effect.
- A radio on another band needs no rule and no second set of settings in the hub: it returns its band.
- A user's `low_power: false` stands against a band whose default is low-power.
- A hub whose Wi-Fi sits on one of the band's channels gives that channel up for requests. With Wi-Fi wider than
  20 MHz the guard is too narrow; the hub sees only its primary channel.
- A request that fails on one channel is retried on another, never on the one that worked last; where one channel is
  much better than the others, the first attempt reaches it one time in the size of the set.

## Considered options

- **A hook per value on the radio driver.** Rejected: each hook is one more rule the hub asks the driver about, and
  the values are data.
- **A second set of tuning keys per band.** Rejected: two keys for one meaning, and the hub would choose between
  them by the radio's name.
