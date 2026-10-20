# ADR 0053: A device may carry its own system key

<!-- doxygen-label: adr0053 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

The hub holds one system key, and every exchange used it. An Atlantic AGILIA IO radiator breaks that
assumption: in its display's pairing mode it refuses a pushed key and hands over its own by pull,
and each radiator has its own (issue #165). The key check value of one radiator's key proves
nothing about the next one. A hub that controls several such devices, or one such device next to
devices on the installation key, needs a key per device.

Three places used the hub key without asking which device was on the other end: the `0x3D` we send
after a device's `0x3C` challenge, the verification of a device's `0x3D` to our challenge, and the
keyed roll-call that Scan Paired Devices broadcasts. A device that keeps its own key answers none of
them under the hub key.

## Options considered

- **A second hub-level key list, matched by address.** Rejected: the key belongs to the device, and
  the entity that declares the device is where a user already writes `io_device_id`.
- **Pass a key to every `send_and_receive()` caller.** Rejected: fourteen call sites in five files
  would each need to find the destination's key, and a new caller could forget.
- **A per-device `system_key:` on the device entity, resolved inside the exchange engine** (chosen).

## Decision

- Every device-bound platform (cover, light, switch, lock, climate) takes an optional `system_key:`
  in its shared schema, validated like the hub's. The registry stores it on the device record
  (`IoDevice::has_system_key` and the key).
- The exchange engine picks the key per frame through one lookup the hub installs at construction
  (`ExchangeEngine::set_device_key_provider()`, the same shape as the target-evidence provider). Our
  `0x3D` is signed with the key of the request's destination. An inbound `0x3D` is verified against
  the key of the request's sender. A node the hub does not know, or one without a key of its own,
  uses the hub key. A per-device key equal to the hub key is accepted silently; it changes nothing.
- The keyed discovery payload and the pushed key stay the hub key: a device that is pushed a key
  joins the hub's key. 1W controller keys are a separate system and are untouched.
- Scan Paired Devices runs the full sweep once per distinct key: the hub key, then each different
  per-device key. The sweep itself (`run_roll_call_sweep()` in `roll_call.h`) takes the key as an
  argument, so any other caller can run it. The report names the key a device answered under by its
  check value, never the key. An unknown device found under a per-device key gets a snippet with
  `system_key:` in the device log, inside a DO-NOT-SHARE block; the event version of the report omits
  the key. A known device that answered under a key other than the one its YAML uses gets a `hint:`
  line.
- There is no per-device controller address. Devices check the key, not the controller's node ID:
  key extraction and issue #165 both control devices from the hub's own node ID.

## Consequences

- **A scan takes six more broadcasts per distinct per-device key** (about 13 seconds each time on a
  default configuration). The sweep logs its key check value first, so the extra time is explained.
- **The per-device key sits in the generated firmware and in the YAML**, like the hub's. `!secret`
  keeps it out of the config file itself.
- **A key printed in a scan report is key material.** The snippet builder takes the key as an
  optional input. The scan prints a snippet that carries one only in the device log, inside a
  banner. The action-result event, which Home Assistant records, carries the same report without
  the key and says the device log has it. No other log line prints a key.
