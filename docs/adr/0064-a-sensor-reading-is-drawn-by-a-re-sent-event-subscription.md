# ADR 0064: A sensor reading is drawn by a re-sent event subscription

<!-- doxygen-label: adr0064 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0060](0060-a-state-changing-diagnostic-probe.md) (point 4, and the consequence that
leaves entities and automatic subscription to later decisions). Follows the pattern of
[ADR 0050](0050-rain-is-read-by-an-opt-in-limitation-poll.md) and amends its miss rule (point 6
below), since the rain poll shares the schedule.

## Context

A Somfy RS100 Solar io motor answers a subscription (`0x73 01 <event>`) at once with a `0x71`
carrying the event's current value (issue #156):

- event `0x2005` carries a sun value: `06 00 <hi> <lo> B9`, 0 at night, 183 and 744 in daylight
  (corpus `somfy_rs100_probe_subscribe_event_sx1262` and
  `somfy_rs100_probe_subscribe_event_daylight_sx1262`). The motor stores the same number in its
  power record (`somfy_rs100_probe_read_object_a601_sx1262`);
- event `0x2001` carries one byte, 100, while the status byte's battery field reads full
  (`somfy_rs100_probe_subscribe_event_battery_sx1262`).

The motor has never sent either event on its own in any log from that issue, so a subscription that
waits for reports would show nothing. Over one afternoon the reporter subscribed every 20 minutes:
15 of 21 subscriptions were answered. The other six got no answer at all: in five the motor took
the request and sent its challenge but no answer followed; in one it never woke. Inside each
exchange, a second try after a silent first one was answered in six of nine cases. The answer to the subscription is the only way the value
arrives, and it costs one exchange. The motor accepted every repeated subscription it was sent
(more than five to `0x2005` over a week, one to `0x2001`).

Three existing rules stood in the way:
- ADR 0060 never sends an accepted subscription twice ("a second accepted copy has no use");
- the hub treats any event report arriving inside an exchange as *not* the reply, records it and
  asks again (ADR 0060's amendment);
- and ADR 0060 left an entity for the reports to a later decision.

## Options considered

- **Wait for pushed reports after one subscription.** Rejected: none has ever been seen.
- **Read the motor's power record with the data-transport read (ADR 0062).** Rejected for a
  periodic sensor: about 25 exchanges and 30 seconds of blocked loop per read, against one
  exchange.
- **Re-send the subscription periodically and read its answer** (chosen).
- **A separate schedule class per sensor.** Rejected: the rain poll's schedule
  (`RainPollPolicy`) had nothing rain-specific but its name; it becomes `PeriodicPollPolicy`, one
  instance per poll.

## Decision

1. **Two opt-in cover keys, no default:**
   - `sun_level_poll_interval` creates `<Cover Name> Sun level`, a sensor with no unit and no
     device class, published raw;
   - `battery_level_poll_interval` creates `<Cover Name> Battery level`, a `%` sensor with
     device class `battery`.

   Each key schedules its own poll; a cover without it is never polled and gets no entity. The
   minimum is one minute, as for the rain poll.
2. **The poll is the subscription.** `create_subscribe_event()` with its existing two-event
   allowlist builds the request; the answer is the reading. Every poll subscribes again.
3. **The answer is the reply.** `decisions::is_subscription_answer()` accepts a well-formed `0x71`
   carrying the subscribed event from the addressed device as the reply to that request. Every
   other event report keeps ADR 0060's handling: recorded, never applied, request asked again.
   The poll's own answer is not put on the Last Device Event sensor.
4. **One re-send after an accepted, unanswered try** (`UNCONFIRMED_SUBSCRIBE_MAX_RESENDS`, for the
   probe as well as the poll). This replaces ADR 0060 point 4: the answer carries the reading, so a
   lost answer is worth one more copy, and the device accepts repeats.
5. **Strict decode.** `decode_sun_level_event()` accepts only the captured shape (5 data bytes,
   first byte `06`, value in bytes 2–3); `decode_battery_level_event()` only one byte up to 100.
   Any other answer is a miss, and the log names its payload once.
6. **Same schedule rules as the rain poll** (ADR 0050):
   - the first poll comes 15 to 45 s after boot, then every interval ±10 %;
   - a moving device is asked again 30 s later;
   - a poll the device leaves unanswered (silence, or a challenge with no answer) is asked once
     more 60 s later (`PERIODIC_POLL_MISS_RETRY_MS`), and counts as a miss only if the retry is
     unanswered too. An error reply or an answer of another shape counts at once, since asking
     again soon would not change it. This applies to the rain poll as well and amends ADR 0050's
     "three consecutive unanswered polls";
   - the reading goes unknown after three consecutive misses;
   - it is a background operation, and pairing flushes it;
   - a miss counts in Exchange Failures, never in the status poll's backoff.
7. **Pushed reports stay unapplied.** A `0x71` with these events that arrives on its own is still
   only recorded (ADR 0060 point 5).

## Consequences

- **Each poll writes the subscription into the device again.** Nothing so far says the device keeps
  more than one entry per controller, but that is not known. The documentation says so plainly.
- **Transmit time is the user's choice**, as for rain. On a `low_power` motor each poll carries the
  long wake-up preamble, often twice, so a one-minute sun poll costs several times the hourly
  allowance of the 868.95 MHz band; the documentation gives the cost per interval and recommends
  values, and the hub enforces only the one-minute floor.
- **A missed poll costs one more exchange a minute later.** At the RS100's miss rate (about one
  poll in four) that adds about a quarter to the poll's transmit time. In exchange, a slot ends
  without a reading only when two polls a minute apart both fail. Replayed against the reporter's
  afternoon, that turns a sun level that would go unknown about once a day at a 30-minute interval
  into one that almost never does. A retried slot moves the schedule by one minute.
- **The sun value has no unit.** Publishing it raw means adding a unit later is not a breaking
  change; a converted value would be a new entity.
- **Percent rests on one value (100).** A level below 100 has not been captured.
- **Confirmed on one device.** Both sensors are experimental: one Somfy RS100 Solar io, one
  reporter. Other devices may refuse the subscription or answer in another shape; their sensors
  then stay unknown.
- **`battery: true` is unchanged.** It reads the coarse full/normal/low level from status replies
  at no radio cost and stays the low-battery signal.
