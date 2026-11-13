# ADR 0076: A moving cover is read once at its predicted end of travel

<!-- doxygen-label: adr0076 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0073](0073-a-read-plans-its-attempts-and-an-attempt-must-fit-the-budget-with-its-windows.md)
and [ADR 0074](0074-settling-reads-take-the-attempt-plan-and-progress-reads-stay-single-tries.md)
(a further read that settles: the predicted-end read) and states the precedence between the reads
that follow a status report.

## Context

A cover that moves reports its remaining travel time in seconds in every status reply (the
remaining-time field of the VELUX KLF 200 API; issue #178 logs show it on two Somfy shutters). The hub
polls such a cover every few seconds with single-try progress reads (ADR 0074). Those land anywhere
in the last seconds of the travel, and a read that lands in the second the motor switches its load
can be lost, so the final position appears a poll late (issue #178). In those logs the motor came to
rest within about 1.3 s of its count reaching zero.

For a remaining time of 2 s or less the rule that was in place already reads at the remaining time
plus 1 s (`settle_delay_ms()`), and a count of 0 in a report that says moving already gives a read
after 1 s, but as a single try. For a count above 2 s the next progress read is the usual 3 s away
and lands wherever it lands. The read at the end of the count is the one most likely to hit the
second in which the motor switches its load and drops a reply (issue #178), so it is the one that
needs the attempts.

## Decision

- **One settling read at the predicted end.** A trusted status reply that reads as moving and carries
  a remaining time R, of any size and 0 included, plans one read for 1 s after R has run out. For
  R of 2 s or less that is the instant of the progress read at R + 1 s, which the settling read
  replaces. A count of 0 is believed only from a cover that has sent a count above 0 before; from any
  other it is no count. The read is a settling read (ADR 0074): it takes the settling plan, so a lost
  request is repeated as often as the exchange budget allows. The aim is R + 1 s without a further margin: the attempts of the plan span the
  second in which the motor switches its load.
- **The aim follows the count.** Each trusted report with a count replaces the aim. A report that
  carries no count (a report that says stopped, or a device that sends none) drops the aim; the
  progress read that report plans stands. An aim past the end of the 600 s follow-up window is not
  planned.
- **A frozen count is given up.** After three reports in a row whose count did not fall below the one
  before, the aim is dropped and only progress reads remain. A count that falls again aims again.
- **Progress reads stay as they are.** They keep their cadence and their single try. A progress read
  that is due within 1 s before the predicted-end read is dropped, since the settling read follows at
  once; one that is due when the predicted-end read is due is dropped too. A due predicted-end read
  wins over everything else the device had due.
- **A report that says stopped schedules from the default delay.** A reply that says stopped never
  schedules the next poll from the count it carries, whatever that count is. The first stopped report
  off the target is confirmed by one more read (ADR 0074), and the verdict rests on that read coming
  no sooner than the default delay (3 s, or the configured interval when shorter): a device that
  answers "stopped, new target, old position" before its motor starts is otherwise taken as at rest.
  So the confirming read is never pulled forward by a count of 0 or a short one, and no predicted-end
  read is planned next to it. This states the precedence between the two rules explicitly.
- **The mark travels like the other settling marks.** The device's tracking holds the aim and the mark
  that makes the popped read a settling read. A newer report, a new command and clearing the tracking
  drop the mark, as they do the marks of ADR 0074.

## Consequences

- **Airtime.** A cover that counts down ends its move on a read that is repeated only when it is
  lost; with a count above 2 s that is one more read per move. A cover that sends no count behaves
  as before.
- **Final position.** The read lands about 1 s after the count ends. A lost first attempt is repeated
  inside the same exchange rather than a poll later.
- **Blocking.** A predicted-end read that is never answered blocks the loop for nearly the whole
  budget, as every settling read does (ADR 0074).
- **Hardware status.** Confirmed on a Somfy Sunea io awning: it reports a remaining time that falls
  about one second per second, and the read at the predicted end finds it at rest (corpus capture
  `somfy_awning_statuspoll_close_with_remaining_time_predicted_end_read_sx1262`, replayed on the timed
  clock). The read stays experimental for other covers until a capture or a reporter's log confirms
  them.
- **Low-power covers.** The read takes the settling plan for a low-power target (three attempts after a
  wake-up preamble); whether the cover is awake at that moment is not decided here.
