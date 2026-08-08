# ADR 0022: Unauthenticated status frames are never applied to device state

**Status:** Accepted · **Recorded:** 2026-08

## Context

`process_received_packet_()` (hub_status.cpp) runs only while the hub is idle — any exchange
this hub itself initiates sets `busy_`, which suppresses this dispatcher for the duration, and
the matching reply is consumed directly inside that blocking call instead. So by construction,
every frame that reaches this dispatcher is one this hub did not ask for.

Two of the branches here used to merge such a frame's claimed position/tilt/target/`is_stopped`
straight into the device registry: every `CMD_PRIVATE_RESP` regardless of `dst`, and any
`CMD_STATUS_UPDATE` not addressed to this hub. Neither is authenticated — unlike the
addressed-`CMD_STATUS_UPDATE` branch right above it, which does gate the merge behind
`authenticate_request_()` before touching the same registry. `update_device_status_()` requires
`registry_.get(src)` to succeed first, so this could only overwrite an already-owned device's
record, not inject a phantom one — but for that population, anyone who knows the device's node
ID (node IDs travel in the clear on the radio) could transmit a spoofed frame and have it
displayed as truth in Home Assistant, with no key needed at all.

The only non-attacker source that could legitimately reach this code at all is a second real 2W
controller sharing a device with this hub — a manufacturer gateway (Somfy TaHoma/Smoove, Velux
KLF200), or a second instance of this project — whose own poll or command to that shared device,
and the device's reply, this hub happens to also receive since RF is a shared medium.

## Options considered

- **Challenge the source before merging**, reusing `authenticate_request_()` (the same call the
  addressed branch already makes). Closes the gap completely, but costs a fresh `0x3C`/`0x3D`
  round-trip for every observed status-bearing frame from a recognized device — real, recurring
  airtime on any installation with more RF activity than a single quiet controller, and it can
  collide with the very exchange it's trying to verify (the same "blind while exchanging" cost
  ADR 0013 already documents elsewhere).
- **Don't trust the content, but schedule this hub's own poll instead** — mirroring the existing
  "remote activity" pattern a few lines below (traffic addressed to one of our devices from an
  unrecognized source already just schedules a poll rather than trusting anything). Still turns a
  passive receive into an active transmission on unauthenticated input: an attacker who knows
  several device IDs could nudge the hub into transmitting more than it otherwise would — bounded
  by the existing per-device debounce (a named timeout that resets rather than stacks), but still
  a new, attacker-triggerable RF cost that did not exist before.
- **Drop the merge, replace it with nothing.** If the legitimate source is a second real
  controller, that controller has its own means of reporting the shared device's state (its own
  Home Assistant integration, if any) independent of whatever this hub happens to overhear. This
  hub declining to also mirror that traffic loses nothing a real second controller was uniquely
  providing.

## Decision

The third option. The frame's content is never applied, and nothing is scheduled in its place:

```cpp
if (frame.cmd == CMD_PRIVATE_RESP || frame.cmd == CMD_STATUS_UPDATE) {
  detail::log_frame_issue(this, "rx", "unauthenticated_status_ignored", frame, packet.len);
  return;
}
```

## Consequences

- No unauthenticated data — from an attacker or otherwise — is ever applied to device state
  again. This closes the gap V50/V58 describe in a sibling project's own component (an
  independent AI-assisted security-hardening pass found the same bug class there): a recorded or
  fabricated frame can no longer move a device's displayed position, tilt, target, or stopped
  state, nor silently clear its "Active Issue" indicator.
- Zero added radio traffic, in both the benign and the adversarial case: this hub does not
  transmit anything in response to a frame it didn't ask for. An attacker spamming spoofed frames
  gets nothing back — no state change, no induced transmission, nothing to observe as feedback
  that the spoof was even received.
- The cost lands entirely on the narrow multi-controller-sharing-a-device scenario: this hub's
  own view of a shared device's position now only updates via its own regular, authenticated
  polling cadence, not instantly from overhearing another controller's exchange.
- Regression-pinned by `HubStatus.UnauthenticatedForeign{PrivateResponse,StatusUpdate}DoesNotUpdateDeviceState`
  (hub_status_test.cpp) — both verified to fail against the prior code before this change landed.
- `authenticate_request_()` remains exactly as expensive and exactly as used as before — this
  decision is about *not* reaching for it here, not about changing its cost anywhere else.
