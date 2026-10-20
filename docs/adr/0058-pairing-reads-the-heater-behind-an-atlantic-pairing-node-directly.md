# ADR 0058: Pairing reads the heater behind an Atlantic pairing node directly

<!-- doxygen-label: adr0058 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

An Atlantic AGILIA IO radiator pairs from a node that presents itself as a remote controller (node
type 1023). That node hands over the radiator's key, but the device to control is the heater one
address up, an electrical heater with an adjustable setpoint (node type 52, subtype 1). ADR 0051 finds
that heater with a roll-call under the key. Three radiators in issue #165 followed the same pattern
(`B6BDFB`/`B6BDFC`, `986110`/`986111`, `F6BE8D`/`F6BE8E`), each with its own key.

The heaters answer a roll-call poorly: 10 of 42 roll-call requests drew a reply, 0.27 to 1.26 s
later, and two whole six-request sweeps drew none
(`tests/corpus/captures/discovery/atlantic_agilia_discovery_heater_rollcall_late_reply.yaml`,
`tests/corpus/captures/pairing/atlantic_agilia_pairing_key_pull_own_key_heater_silent.yaml`). A missed
heater left the user without a YAML block: with a foreign key the attempt ended `key_received`, and
with the hub's own key the pairing node itself was registered as a device that cannot be controlled,
after a SetConfig1 it refuses with `FE 08`. The same heaters answered every authenticated register
read in the climate test.

## Decision

1. **Read the next address first.** After a proven pull from a remote-controller node whose discovery
   reply names manufacturer 12 (Atlantic Group), the hub reads the maximum-setpoint register (`0C 60 01
   02`) of the address one above, signed with the pulled key. An answer is taken as the electrical
   heater with an adjustable setpoint, the only device that speaks this register protocol, and it is
   reported and registered exactly as a roll-call responder would be. The read is read-only and uses
   the normal exchange retry budget. The predicate and the address arithmetic are pure functions in
   `hub_decisions.h`.
2. **The roll-call stays the fallback** when the read gets no answer, and the only search for any other
   kind of device. A device the YAML already declares at that address is not probed.
3. **A hub-key pull that finds no device ends as `hub_key_returned`**, with the advice to run Scan
   Paired Devices. The pairing node is not registered and gets no SetConfig1.
4. **A device a pairing registered is marked as such** (`registered_by_pairing`). Reports still print
   its block and a later pairing replaces its record, so pairing the same radiator twice in one boot
   ends `paired` both times.
5. **The key proof is retried.** The challenge that proves a pulled key is sent up to three times while
   no answer comes back; an answer that does not verify ends the pull at once.

## Considered

- **Repeat the roll-call when it hears nothing.** Rejected: about 13 s per sweep, and with one reply in
  four per request it still misses a share of the heaters.
- **Read the next address for every remote-controller node.** Rejected: only Atlantic radiators are
  known to put the device there, and a read to an unrelated node would be a guess on the air.
- **Probe with the name read (`0x50`).** Rejected: it would answer from any device and not show that
  the node is a heater; the register read does.

## Consequences

- The direct read has not run on hardware yet. On the timing of the issue #165 logs, an AGILIA pairing
  takes about 3 s instead of about 15 s, and the radiator-1 case (hub key, silent roll-call) finds the
  heater through the read.
- The heater's report line carries no manufacturer, turnaround or power-save fields, because no
  discovery reply of its own was read.
- The +1 rule is unconfirmed across a byte carry (`xxxxFF`); the read then simply goes unanswered and
  the roll-call runs.
- A radiator model whose heater speaks another protocol, or sits elsewhere, falls back to the roll-call
  exactly as before.
