# ADR 0100: A transfer read tells an accepted request without an answer from silence

<!-- doxygen-label: adr0100 -->

**Status:** Accepted, amends [ADR 0099](0099-scheduled-transfer-reads-use-one-session-and-a-queued-reader.md) · **Recorded:** 2026-11

## Context

[ADR 0099](0099-scheduled-transfer-reads-use-one-session-and-a-queued-reader.md) moved the
`read_object` probe onto the hub's recorded exchange, which answered only "a reply came" or "it did
not". A device that challenges a request has received it; when it then sends no answer, the link
works, and either the device has no data for the request or it did not accept the hub's answer to the
challenge (the hub cannot see which). Plain silence instead means the device did not hear the request
or ignores it. The probe told the two apart before ADR 0099 and lost that, and scanning a data
object entry by entry depends on it.

## Decision

- The hub's recorded exchange reports three results: a reply, a request the device accepted without
  answering, and silence. The exchange's bookkeeping (counters, link health, backoff) is unchanged.
- A transfer read that ends on an accepted open without an answer ends as "accepted, no answer", apart
  from "no answer"; a read that ends on an accepted block request without an answer keeps its outcome
  and says so.
- The probe's messages name the case again: an accepted request without a response, or no reply.

## Consequences

- The Consequences line of ADR 0099 that says the probe no longer tells the two apart no longer holds.
- A caller of the scheduled reader can tell "the device heard the request and answered nothing" from
  "the device did not answer at all"; how a reading treats each is decided where the reading is defined.
