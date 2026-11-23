# ADR 0133: Pairing and the key pull open their sessions through channel access

<!-- doxygen-label: adr0133 -->

**Status:** Accepted, amends [ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md) · **Recorded:** 2026-11

## Context

[ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md) keeps the
hub's requests off a busy channel: a request waits for a free channel, or ends its attempt as "channel busy",
instead of being sent after five checks. It left discovery, pairing, the key pull and broadcasts as they were:
channel 2, five checks 5 ms apart, then a send. A pairing step or a key pull sent into a busy channel collides
in the same way a poll did in [issue #209](https://github.com/laberning/home_io_control/issues/209), at the
moment the user holds a device in pairing mode or a controller in key-sharing mode.

[ADR 0132](0132-a-radio-describes-its-band-and-the-hubs-rules-read-it.md) made the channels a request may use
one set that the radio's band gives and the hub's Wi-Fi narrows. On 868 MHz the set of a low-power request is
channel 2. A step that takes its channel from that set follows both rules without a rule of its own.

While pairing, the hub does not know whether the target sleeps, so a pairing step has to reach a battery
device as well as a mains one. The hub listens for an answer on the channel it sent on (ADR 0096), and the
later frames of a session stay on the channel of its first frame.

## Decision

- **Every addressed frame that opens a pairing or key-pull session goes through the exchange engine's
  channel access**, the same as a request ([ADR 0096](0096-the-hubs-requests-take-a-free-channel-and-are-never-forced-onto-a-busy-one.md)):
  it is never sent onto a channel measured busy, and where the set has more than one channel the next try
  takes another one. A step whose channel stays busy ends like a try that got no answer. A user who set
  `lbt_force_when_busy` gets the same forced send on the quietest channel of the set as a request does.
- **The step's channels are the band's wake-up channels**, the set of a low-power request
  ([ADR 0132](0132-a-radio-describes-its-band-and-the-hubs-rules-read-it.md)), because the target may sleep.
  On 868 MHz that is channel 2, so the steps stay on channel 2 and now wait for it to be free as a
  low-power request does.
- **The rest of the session follows the opener**: every later frame of that session and every wait for its
  answers use the channel the opening frame went out on.
- **No pairing or key-pull code picks a channel itself.** The engine's option for a fixed channel is removed.
- **A discovery broadcast keeps its rule**, with its channels limited to those the band's set allows, so a
  channel excluded near the hub's Wi-Fi is never used.
- **Unchanged:** answers inside a session (no check, at once) and one-way bursts (channel 2).

## Consequences

- Pairing and the key pull no longer collide with a remote or another controller on channel 2; a step can
  take up to about 600 ms longer when channel 2 is busy, and can end as "channel busy" where it was sent before.
- On 868 MHz nothing changes on air while channel 2 is free.
- On a band whose wake-up set has three channels, pairing and the key pull spread over them and avoid the
  channels near the hub's Wi-Fi without further code.
- Whether a device in pairing mode on 868 MHz also listens on channels 1 and 3 stays open; if it does, using
  them is a change of one band value.

## Considered options

- **Keep channel 2 with five checks, then send.** Rejected: it is the behaviour ADR 0096 removed for requests,
  and on another band channel 2 can sit inside the hub's own Wi-Fi.
- **All three channels for every step.** Rejected for now: the hub does not know whether the target sleeps,
  and a battery receiver is only known to listen on the wake-up channels.
