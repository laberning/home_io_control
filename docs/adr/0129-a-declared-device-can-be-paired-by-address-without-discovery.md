# ADR 0129: A declared device can be paired by address, without discovery

<!-- doxygen-label: adr0129 -->

**Status:** Accepted · **Recorded:** 2026-11

Builds on [ADR 0053](0053-a-device-may-carry-its-own-system-key.md) (a device may carry its own
system key) and [ADR 0018](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md) (the YAML is the
only source of truth).

## Context

Discover & Pair starts with a broadcast discovery. A device that already holds another two-way hub's
key answers none: reports of a claimed device that never answered were resolved by key extraction
instead ([issue #98](https://github.com/laberning/home_io_control/issues/98),
[issue #103](https://github.com/laberning/home_io_control/issues/103)), and a key this hub left on a
motor itself had the same effect
([issue #161](https://github.com/laberning/home_io_control/issues/161)). Key extraction needs the
user's own hub and brings its whole installation over.

Everything in the pairing flow after discovery is addressed to one device: the `0x31` key init, the
device's `0x3C` challenge, the `0x32` key transfer and its `0x33` confirm. The discovery answer only
supplies the device's address, kind and power class, and an entity in the YAML carries all three.
A Somfy RS100 io that held a TaHoma's key answered no discovery, with its learn window open or not,
yet took a key pushed directly to its address in its power-cycle learn window and obeyed authenticated
commands afterwards ([pull request #257](https://github.com/laberning/home_io_control/pull/257)).

The key transfer also carried the hub key unconditionally, while every later exchange uses the key the
exchange engine picks for the device, its own `system_key:` first (ADR 0053). A device declared with
its own key was given the hub key and then refused every command.

## Decision

- **A `pair_device` action** (native API, `device_id`) runs the key-exchange phase alone, addressed to
  one device, with the kind and power class of its YAML entry in place of a discovery answer. It sends
  no broadcast. It is a user feature, not gated by `diagnostic_probes:`.
- **Only a declared device.** A device no entity declares, or one only an earlier pairing registered,
  is refused before anything is sent. The entry is where the kind and power class come from, and it
  keeps the action aimed at the user's own devices.
- **Push only.** The action refuses when the key transfer direction is pull, or when the declared kind
  hands out its own key: a pull needs the discovery answer this route skips, and such devices answer
  discovery anyway.
- **The pushed key is the key the hub then uses.** Every key transfer, in this route and in Discover &
  Pair, carries the key the exchange engine authenticates that device with: the entity's
  `system_key:` when it declares one, the hub key otherwise.
- **Success is the device's `0x33` confirm.** The result is never `verified`, since nothing reads a key
  back out of a device. The attempt is reported like a Discover & Pair attempt ("Last Pairing
  Result"); an action refused before anything is sent reports no attempt.

## Consequences

- A claimed device can be moved to this hub without touching the hub that holds it and without taking
  that hub's key.
- The hub that held the device loses it: the device now holds this hub's key, and that cannot be
  undone from here. The docs warn about this.
- The device's own pairing window still has to be open, by the gesture its manual describes. Nothing
  in this route can open it; outside it the action reports no confirm.
- The user writes the entity by hand first (address from the `io_capture` log tag, kind from the
  device), as for any device discovery cannot reach.
- Devices that share a circuit can be paired one after another in one window, since nothing is
  broadcast.
- Only one device kind has confirmed the route so far (the RS100 io); other kinds may gate the key
  transfer differently in their learn window.

## Considered options

- **Gate the action behind `diagnostic_probes:`.** Rejected: it sends only the frames Discover & Pair
  already sends, to a device the user declared, and it is the route the docs recommend for a claimed
  device.
- **Accept any address.** Rejected: without an entry the kind and power class would be guesses, and
  the action could be aimed at a device that is not the user's.
- **Run a key pull by address as well.** Rejected: a device that hands out its own key answers
  discovery, so Discover & Pair already reaches it.
