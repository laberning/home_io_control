# ADR 0130: A controller's key can be pulled by its node id, without discovery

<!-- doxygen-label: adr0130 -->

**Status:** Accepted · **Recorded:** 2026-11

Builds on [ADR 0071](0071-a-pulled-key-without-an-answer-is-proven-by-the-devices-that-hold-it.md) (how a pulled key
is proven) and [ADR 0129](0129-a-declared-device-can-be-paired-by-address-without-discovery.md) (the push side of
pairing without discovery).

## Context

Discover & Pair pulls a key only from a node that answered its broadcast discovery. Everything in the pull after
discovery is addressed to one node: the `0x38` request, the node's `0x32` with the key, the hub's `0x3C` challenge and
the node's `0x3D`. The discovery answer supplies only the address and the kind. The pull is hardware-validated on an
Atlantic AGILIA IO radiator ([issue #165](https://github.com/laberning/home_io_control/issues/165), corpus
`atlantic_agilia_pairing_key_pull_heater_rollcall`).

A controller that shares its key does so in a mode the user starts on it, for a short time, and it need not answer a
discovery in that mode. A user who knows the controller's node id has no way to ask it directly. The node id of a
controller travels in the clear in every request it sends, but the log does not name it.

Some nodes hand over a key and then answer none of the hub's challenges
([issue #215](https://github.com/laberning/home_io_control/issues/215)); ADR 0071 proves such a key by the devices
that answer a roll-call under it.

[ADR 0129](0129-a-declared-device-can-be-paired-by-address-without-discovery.md) already runs the push side of pairing
for a device named by address.

## Decision

- **A `pull_key` action** (native API, `controller_id`) runs the key pull alone, addressed to the node it names. It
  sends no broadcast discovery. It is a user feature, not gated by `diagnostic_probes:`.
- **The pull is the one Discover & Pair runs.** The action reuses the same step with the same attempts, windows and
  proof; there is no second implementation of the sequence.
- **A proven key ends the action.** The key is printed once in the device log with its check value; the Home Assistant
  event and the pairing result carry the check value and the advice, never the key. Nothing is registered and nothing
  further is sent.
- **A key the node does not prove is proven as in Discover & Pair** (ADR 0071): by a roll-call under it, with the
  devices that answer reported the same way. A key nobody proves is not printed.
- **The hub's own key coming back** is reported as "already shares this hub's key".
- **Refused before anything is sent:** an uninitialised hub, an id that is not six hexadecimal digits, the hub's own
  id. A refused action reports no pairing attempt.
- **The attempt is reported like a pairing attempt** ("Last Pairing Result"), with advice codes of its own: proven,
  own key, refused with the node's result code, unanswered.
- **The log names an overheard controller.** A request from a node that is neither the hub nor a registered device is
  logged at INFO with its node id, each source at most once in ten minutes, so a user reads the id by pressing a
  button on the controller.

## Consequences

- A key can be taken over from a controller that does not answer discovery, without the hub searching for devices.
- The user needs the controller's node id and has to start the controller's key-sharing mode first; outside it the
  action reports a refusal or no key.
- The route is confirmed on hardware only for nodes that hand out their key when asked (issue #165, issue #215). A
  vendor remote in its key-sharing mode is untested; the docs say so until a report confirms it.
- The overheard-controller line adds at most one INFO line per foreign controller in ten minutes to the log of a hub
  that has neighbours.
