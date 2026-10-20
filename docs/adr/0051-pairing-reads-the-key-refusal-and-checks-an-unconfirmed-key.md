# ADR 0051: Pairing reads the key refusal and checks an unconfirmed key

<!-- doxygen-label: adr0051 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

Three field reports ended at the key step of Discover & Pair, each in a way the pairing flow handled
badly.

- An Atlantic AGILIA IO radiator challenged our key init and answered the key transfer (0x32) with
  `FE 08` (issue #165, corpus `atlantic_agilia_pairing_key_transfer_command_unknown`). The log called
  `0x08` an execution error, and the hub then re-sent the key init.
- A Somfy J406io answered a re-sent key transfer with `FE 80` and was reported `key_exchange_failed`.
  A later roll-call showed it held the hub's key, and Home Assistant controlled it (issue #161).
- A Somfy Sunilus answered the key transfer with `FE 80` on every retry (issue #65, corpus
  `somfy_sunilus_pairing_key_transfer_rejected_error`).

The `CMD_ERROR_RESP` byte packs a result code into bits 7..3 and an execution state into bits 2..0.
Our captures fit that layout. Every corpus `0xFE` byte but one has state 0, a device answers a
command it does not implement with `0x08` (code 1, "command unknown"), and a priority lock comes
back as `0x38` (code 7). The table the hub used looked the byte up unshifted, so `0x08` read as
"error during execution" and `0x80` had no name.

The radiator's discovery reply also carries node type 1023 (`FF C0`, a remote controller), not a
heater type. `DeviceType` held 8 bits and truncated it to `0xFF`, so nothing noticed;
[ADR 0052](0052-device-types-follow-the-io-node-type-and-subtype-table.md) widens it to the full
ten-bit field.

## Options considered

- **Keep retrying after a refusal.** Rejected: a refusal is the device's answer. Repeating the push
  draws the same refusal, and in #161 the second key transfer drew a refusal the device had no
  reason to send.
- **Treat any unconfirmed key exchange as a failure.** Rejected: #161 shows a device that holds our
  key while every logged attempt says it failed.
- **Switch to the other direction inside one attempt when a push is refused.** Rejected: a refusal
  repeats deterministically, so the second direction would cost pairing time on a device that the
  discovery answer already identified.
- **Report the key step as it really ended, check an unconfirmed key before failing, and choose the
  direction from the discovery answer, with a pull for devices that refuse a pushed key** (chosen).

## Decision

- `command_result_name()` reads a byte with state 0 as a packed code (`RESULT_COMMAND_UNKNOWN` =
  `0x08` .. `RESULT_ALREADY_CONFIGURED` = `0x88`). A byte with state bits set keeps the run-status
  and limitation table, because a Somfy awning does answer with `0x05` (corpus
  `somfy_awning_identify_wrong_systemkey_sx1276`). Six old names whose bytes are multiples of 8 are
  replaced. The pairing log prints the raw byte, the name, the code and the state.
- A `CMD_ERROR_RESP` answer to the key transfer ends the attempt. It sends no key-init re-send and
  no further key-exchange retry. The outcome is `key_transfer_refused`, a new value next to
  `key_exchange_failed`.
- When the key exchange did not confirm and the device challenged our key init, the hub sends one
  authenticated name read (0x50, two tries). Only a device that challenges the read and then answers
  with its name (0x51) counts as holding our key. That device is registered as paired.
- Discovery decodes the full 10-bit node type and logs a warning when a device answers as a remote
  controller.
- The pairing advisor reports `key_push_not_accepted` when the device answered the push with
  "command unknown" or answered discovery as a remote controller, and `key_refused` (with the code)
  for any other refusal.
- `pairing_key_transfer: auto | push | pull` (default `auto`) picks the direction. `auto` pulls from
  a device that answered discovery as a remote controller (node type 1023) and pushes to any other
  device; `push` and `pull` force it. The hub never switches direction inside one attempt. With `auto`
  a remote controller therefore never gets the push suggestion, and the advisor's push and pull
  advice both name the setting that forces the other direction.
- A pull sends a key transfer request (0x38) with six random bytes, and the device answers with a
  0x32 holding its key under the transfer key. The IV's data is the whole request (`0x38` and the six
  bytes) and its challenge the same six bytes. The hub then challenges the device (0x3C), and the
  device's answer (0x3D) is a MAC over its own 0x32 with its own key. A host test checks this
  construction against a documented pull exchange, including the 0x3D, and an Atlantic AGILIA IO
  radiator proves it on air (issue #165).
- A pulled key equal to the hub's own is accepted: no decrypt produces it by chance, and the device is
  paired as after a push. When the node that answered has no platform of its own, the hub first runs
  the same roll-call under the hub key and registers the responders with a platform (they use the hub
  key and carry none of their own); with no responder it keeps the plain paired path. A different key must verify the device's 0x3D. The hub then runs one
  roll-call sweep under the pulled key (`run_roll_call_sweep()`), because the node that hands a key
  over is not necessarily the node that holds it as an actuator: the AGILIA's heater is the next
  address up. Every responder with a platform that the registry does not know yet is registered with the
  pulled key as its own `system_key` ([ADR 0053](0053-a-device-may-carry-its-own-system-key.md)), so it
  can be controlled until the YAML is flashed. The outcome is `paired` when at least one device was
  registered and `key_received` when none was (advice `pulled_key_received`).
- The device log gets one report for the whole pull, framed by a single DO-NOT-SHARE banner: a
  paste-ready block per responder with its `system_key:`, "not supported yet" and the key for a type
  no platform drives, or the key once when nothing answered. The key appears there only. The Last
  Pairing Result sensor (`node=` stays the node that was discovered), the pairing summary and every
  Home Assistant event carry the key's check value at most (the first three bytes of the key's AES
  encryption of a zero block), so a user can post them and compare two attempts or two devices. An
  unproven key is not printed at all. The advisor reports `key_pull_not_offered` when the device
  answers the request with an error, and `key_pull_unproven` when it sends a key it does not prove.

## Consequences

- **Error names change in Home Assistant.** The Active Issue sensor and the management-action
  messages show the new names for `0x08`, `0x10`, `0x18`, `0x20` and `0x58`. Automations that match
  the old strings need updating.
- **A failing pairing attempt can take up to two name-read tries longer.** The check runs only on an
  attempt that is already failing, after a challenge.
- **A refused attempt ends sooner.** A slow-turnaround radio no longer re-sends the key init twice
  after a refusal.
- **The pull is proven on one device.** An Atlantic AGILIA IO radiator hands over its key and
  proves it with its 0x3D under the documented construction (issue #165). A Somfy Izymo dimmer
  answers the request with `COMMAND_UNKNOWN`, so actuators of that kind offer no pull, and `auto`
  never asks them: only a remote-controller answer pulls.
- **A pull adds a roll-call to the attempt.** It costs one sweep (about 13 seconds on a default
  configuration) on top of the pull itself.
- **A pulled key never replaces the hub's.** A hub keeps one system key for its own installation;
  a device with a different key carries it as its own, so devices on the old key keep answering.
