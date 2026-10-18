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
heater type. `DeviceType` is 8 bits wide and truncated it to `0xFF`, so nothing noticed.

## Options considered

- **Keep retrying after a refusal.** Rejected: a refusal is the device's answer. Repeating the push
  draws the same refusal, and in #161 the second key transfer drew a refusal the device had no
  reason to send.
- **Treat any unconfirmed key exchange as a failure.** Rejected: #161 shows a device that holds our
  key while every logged attempt says it failed.
- **Report the key step as it really ended, check an unconfirmed key before failing, and add an
  opt-in pull for devices that refuse a pushed key** (chosen).

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
- `pairing_key_transfer: pull` (default `push`) is an experimental alternative to the push. The
  hub sends a key transfer request (0x38) with six random bytes, and the device answers with a 0x32
  holding its key. The hub decrypts it with the IV built from `{0x38}` and those six bytes. It then
  challenges the device (0x3C) to prove it holds that key. A key equal to the hub's own is accepted:
  the decrypt cannot produce it by chance. A different key is printed for the user and never adopted
  (outcome `key_received`).

## Consequences

- **Error names change in Home Assistant.** The Active Issue sensor and the management-action
  messages show the new names for `0x08`, `0x10`, `0x18`, `0x20` and `0x58`. Automations that match
  the old strings need updating.
- **A failing pairing attempt can take up to two name-read tries longer.** The check runs only on an
  attempt that is already failing, after a challenge.
- **A refused attempt ends sooner.** A slow-turnaround radio no longer re-sends the key init twice
  after a refusal.
- **The pull is not validated on hardware.** The request shape and the IV are unconfirmed on air.
  The first bench check is a device that already holds the hub's key: it must hand back the hub's
  own key. Until then the documentation marks the tunable experimental.
- **A pulled foreign key costs the other devices.** A hub has one system key. Adopting a device's
  key, which the user does by hand, stops every device paired with the old key from answering.
