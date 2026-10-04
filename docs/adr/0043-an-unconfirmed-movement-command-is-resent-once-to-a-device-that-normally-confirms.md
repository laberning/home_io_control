# ADR 0043: An unconfirmed movement command is re-sent once to a device that normally confirms

<!-- doxygen-label: adr0043 -->

**Status:** Proposed · **Recorded:** 2026-09

## Context

An authenticated command runs in four frames: the hub sends the command, the device answers with
a challenge (`0x3C`), the hub answers the challenge (`0x3D`), and the device closes the exchange
with a reply. When the challenge arrives and the closing reply does not, the exchange ends
*accepted without a closing reply* (`ExchangeOutcome::SUCCESS_UNCONFIRMED`). For a movement
command (`CMD_EXECUTE`) the hub counted that as delivered and never sent the command again.

That rule rested on one kind of device. Some devices never close an EXECUTE exchange within the
response window at all: they carry out the command and report later through their own status
update. Corpus `somfy_rs100_exchange_near_sx1262` shows the shape: an EXECUTE answered by a
`0x71` status update instead of a closing reply. For such a device, silence is normal, and sending
the command again only repeats what it is already doing.

Issue #119 showed the other case. A Somfy SUNEA awning challenged a STOP within 25 ms, so it had
the command, and one second later it was still moving towards its old target. The hub's challenge
answer never arrived, so the device never carried out the STOP, and the hub had reported it as
delivered. The same installation loses the closing reply on a few percent of commands to devices
that close their exchanges normally, a SUNEA and a LightVar dimmer. So silence after a challenge
means two different things, depending on the device.

The engine already had the retry machinery: an unconfirmed status poll or name read spends its
remaining tries, with a new challenge each time. Only `CMD_EXECUTE` was excluded.

## Options considered

- **Re-send every unconfirmed EXECUTE.** Rejected: to a device that never closes its exchanges,
  every command would go out up to three times, costing about 2.3 s of blocked loop and three
  times the airtime, for nothing.
- **Verify, then re-send.** Use the status poll that already follows a command, and re-send if
  it contradicts the command. Rejected: it needs a per-command rule for what "took effect" means,
  and the poll is lost under the same conditions that lost the reply, so it fails silently exactly
  when it is needed. It also costs a whole extra exchange before the command itself can be sent
  again.
- **Re-send STOP only.** Rejected as too narrow: a lost position or light command is the same
  fault with the same fix, only less urgent.
- **A list of devices that confirm, or one kept in flash.** Rejected: the list would have to cover
  every device on the market and be maintained, and the hub persists nothing it can learn again
  ([ADR 0018](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md)).
- **Learn it at runtime, re-send once to a device that normally confirms** (chosen).

## Decision

- **The device record learns whether it confirms.** `IoDevice::confirms_execute` is set the first
  time the device answers a `CMD_EXECUTE` with a reply that closes the exchange, a status or an
  error alike (`detail::record_exchange_outcome()`). It lives in RAM only and is never stored.
- **The engine reads it through the per-target evidence.** `decisions::TargetEvidence` carries the
  flag, handed over by the provider the hub already installs for the wake belief
  ([ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md)). The engine keeps knowing
  nothing about devices.
- **One pure rule decides.** `decisions::retry_after_unconfirmed_accept_is_safe()` lets every
  non-EXECUTE request keep its full retry budget, as before. It lets a `CMD_EXECUTE` spend one more
  try only when all three hold:
  - the target is known to confirm;
  - the command is repeatable (`decisions::is_repeatable_execute()`): every EXECUTE this hub sends
    names an absolute target, except the stored-position selector used by favourite and vent. On a
    Somfy motor "My" while moving means stop, so a second copy could undo the first;
  - this is the first unconfirmed try of the exchange (`UNCONFIRMED_EXECUTE_MAX_RESENDS` = 1).
- **The re-send is an ordinary try** inside the exchange's existing retry and time budget, with a
  longer gap before it: `UNCONFIRMED_EXECUTE_RESEND_DELAY_MS` (750 ms) instead of the usual 250 ms,
  so it goes out about 1.3 s after the first copy. A device that did act on the first copy is often
  deaf while its motor or load switches. On a Somfy awning, re-sends 0.77 s after the first copy
  drew no challenge at all in 3 of 14 cases. It carries a fresh challenge and answer. A re-send that
  could not start inside the exchange budget is not attempted, and not waited for. It is also the last copy: if the re-send draws no answer at all, the
  ordinary failure retry does not add a third one.
- **A re-send that is answered ends the exchange confirmed**, so the Unconfirmed Exchanges counter
  does not count it. The hub logs `Try N accepted without a closing reply … re-sending` with the
  final-wait reception counts, and that line is the record of the lost first reply.
- **When the re-send is also unconfirmed**, the outcome is what it was before this decision:
  accepted, counted in Unconfirmed Exchanges, and corrected by the confirmation poll.

## Consequences

- **A device that normally confirms gets a second chance at every lost reply**, including the STOP
  in issue #119. A device that never confirms behaves exactly as before.
- **The first command to each device after a reboot is not protected.** Nothing is known about the
  device until it has closed one EXECUTE exchange. This is accepted in exchange for never
  maintaining or storing a device list.
- **A re-sent command reaches a device that already acted** when it was only the reply that got
  lost. Every command this applies to is absolute, so the second copy changes nothing.
- **Airtime and loop time rise only on the anomaly**: one extra exchange per unconfirmed command to
  a confirming device. On a Somfy awning on the bench that was 12-30 % of commands, depending on the
  response preamble; the exchange then blocks the loop for up to about 2.2 s.
- **A lost STOP is repeated about 1.3 s later, not immediately.** The awning runs on for that long,
  which is still far better than never.
- **Proposed until tested on hardware**: repeated STOPs to a moving Somfy awning, counting how many
  needed the re-send and whether each one took effect.

See also [ADR 0013](0013-blocking-exchange-on-the-esphome-loop.md), whose exchange budget bounds the
re-send.
