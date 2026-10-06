# ADR 0050: Rain is read by an opt-in limitation poll

<!-- doxygen-label: adr0050 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

A VELUX window with a wired rain sensor limits its own opening while it rains. The rain sensor
talks only to the window, so a controller learns of it only by asking (KLF 200 API specification
v3.18, section 8.1). A VELUX KLF 200 asks with a limitation read (request `0x25`, reply `0x26`,
section 10.5), and Home Assistant's VELUX integration calls the result rain when the resulting
minimum limit is 89 % or more (observed 89, 91, 93 and 100 %).

ADR 0049 derives a rain flag from ordinary status replies. It sees rain only when the window had to
move or clamped a command, so a window at rest during a shower shows nothing. It rejected a timer
that polls every window; that was a default cost on every install for a state most installs do not
have.

## Options considered

- **Poll every window always.** Rejected: transmit time on a shared band for every install, whether
  or not a rain sensor is wired.
- **Poll on an opt-in per-cover interval** (chosen).
- **A Home Assistant action the user schedules.** Rejected: the schedule would live in an
  automation, outside the hub that knows which windows are busy and how much it has transmitted.
- **Extend the status poll to also read the limitation.** Rejected: the status poll's cadence is
  chosen for movement tracking, not for a reading that changes with the weather.

## Decision

- A cover that declares `rain_sensor_poll_interval` (minimum one minute, no default) is polled on
  that interval and gets a `<Cover Name> Rain sensor` binary sensor. A cover without the key is
  never polled and gets no entity.
- The poll reads the minimum limitation only: one request instead of the gateway's pair.
- `decisions::rain_state_from_limitation_reply()` decides: the main parameter's value at or above
  89 % is rain, anything below is dry, and a value that is not a percentage is unknown. The
  originator and remaining time are logged but are not part of the rule.
- The state is unknown until the first answer and again after three consecutive unanswered polls.
- `RainPollPolicy` owns the schedule. Every poll is scheduled with a random offset of up to 10 % of
  the interval, and the first poll after boot is spread over 30 seconds, so devices that share an
  interval do not transmit together.
- A moving device is not asked; the poll is retried 30 seconds later.
- The poll is a background operation: commands and 1W activity go first, and pairing flushes it.
- The reading is independent of ADR 0049's `limited_by_rain` flag and of the Active Issue sensor.

## Consequences

- **Transmit time is the user's choice and is not enforced.** The documentation gives the cost per
  interval; the hub does not stop a one-minute poll on a `low_power` device from using more than
  the hourly allowance.
- **Latency equals the interval.** A shower that starts just after a poll shows up at the next one.
- **A missed poll counts in the device's Exchange Failures** like any other unanswered request, and
  it never touches the status poll's backoff ladder.
- **The reply is accepted only inside an exchange the hub started** (ADR 0022). A VELUX window
  answers the read without a challenge; a Somfy awning challenges it and answers after the exchange
  engine's usual authentication. An overheard `0x26` is still only logged, and a poll never changes
  a device's position, target or movement state.
- **The reply layout under rain is an assumption** carried by one function and one constant, so a
  captured wet reply that differs changes one place.
- **Other manufacturers are not supported.** A Somfy Sunea awning answers the read, but its reply
  does not fit the layout the rule decodes (the first data byte is `05`, not the main parameter).
  The rule reads that as unknown, so the sensor stays without state on such a device.
