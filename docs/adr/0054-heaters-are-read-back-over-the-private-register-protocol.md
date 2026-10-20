# ADR 0054: Heaters are read back over the private register protocol

<!-- doxygen-label: adr0054 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

ADR 0033 gave heating a write-only send path: a `CMD_WRITE_PRIVATE` (0x20) frame, an
acknowledgement (0x21) as the only feedback, and an entity that showed what it had last sent. That
premise held while the registers were known only as writes.

An Atlantic AGILIA IO radiator (node type 52, subtype 1; issue #165) is expected to answer a
register *read* (`0C 60 01 <reg>`) with the same 0x21 command, echoing the header followed by the
value. The write acknowledgement in corpus `atlantic_thermor_exchange_write_private_param` has that
shape, and the public example lines of the register protocol show reads as `0c60 <reg> <value>`.
The registers an entity needs are expected to be readable: the room temperature (0x0115, signed
hundredths of a degree, raw `-1` for a sensor fault), the comfort setpoint (0x0103), the operating
mode (0x0100), the regulator state (0x0117: none, standby, heating) and the heater's own maximum
setpoint (0x0102). The window sensor (0x010E) is read-only; the register that switches open-window
detection on and off is 0x0106. With reads available, a card that shows only the last command is
less than the device offers, and a setpoint the heater clamps would show a value that was never
held.

## Decision

1. **One register table.** `proto_heating` describes every register once: its access
   (read-only, read/write, write-only) and its value encoding. The same row builds the get payload,
   the set payload and the decoder of the reply. A write to a read-only register is refused by the
   codec, so the window sensor cannot be written.

2. **One exchange helper, two operations.** `send_heating_command()` writes (or reads, for a
   read-only register the `heating_control` action runs) and `read_heating_register()` reads and
   decodes. Both frame a payload and hand it to one private exchange helper, which keeps the
   outcome bookkeeping ADR 0033 required: link health, exchange counters, error result codes.

3. **State is observed.** A decoded reply is stored in `IoDevice::heating` and the entity callbacks
   run; the climate entity publishes from there. Nothing predicts a value: the entity keeps
   showing the last commanded mode and setpoint only until the first read succeeds, and shows
   neither current temperature nor action before then. The presence preset (0x0110) is write-only,
   so it stays last-commanded. ADR 0030's overlay stays out of heating: a prediction that is never
   made needs no withdrawal.

4. **The hub polls the heater.** A climate entity gets a status poll, default every five minutes
   (`status_poll_interval`, at least 60 s), plus one poll shortly after boot. A poll reads the room
   temperature, setpoint, mode and regulator state — four exchanges, stopping at the first failure
   — and the maximum setpoint until it has been read once. A failed poll moves its retry onto the
   `StatusPollPolicy` backoff ladder, and the regular cadence continues either way.

5. **A write is confirmed by a read.** After an accepted mode or setpoint write the entity reads
   that register back and shows the heater's answer, so a setpoint the heater clamped shows as
   clamped.

6. **The window function is the detection setting.** `set_window` wrote the read-only sensor
   register and is gone. `set_open_window_detection` (`on` | `off`) writes 0x0106, and
   `window_open` reads the sensor into the action's result message.

ADR 0033's send-path decision (a plain `send_and_receive_()` on the command channel, the
capability predicate gating every entry point, no use of `execute_request_and_update_()`) stands.

## Considered

- **Keep publishing the commanded value and poll separately for a second set of sensors.** Rejected:
  two sources for one value disagree the first time the heater clamps or the user turns the
  dial; the card must show one.
- **Predict the setpoint and withdraw it on a failed read-back.** Rejected: a heater that does not
  answer reads is exactly the case where the prediction would be wrong and stay on screen.
- **Poll at the cover cadence.** Rejected: a heater's state moves over minutes, and each poll costs
  four exchanges of airtime.

## Consequences

- The reply shape of a *read* (header echo followed by the value) is confirmed in the corpus only
  for the write acknowledgement; it is unconfirmed on real hardware until a capture arrives. A
  reply that does not decode is logged at DEBUG and the poll counts as failed.
- A climate entity bound without `io_device_type:` polls with the generic status request until
  discovery resolves the type, as the other platforms do.
- The meanings of 0x0117, 0x0106 and 0x010E, and the hundredths encoding of 0x0115, are experimental
  until a capture or issue #165 confirms them.
- A background poll uses the scheduled-poll try band (the retry ladder is its retry); a read the
  user waits on, such as the read-back of a write, keeps the full retry budget.
- The regular-cadence timer has its own per-device id, so a poll that a linked remote triggers
  cannot cancel it.
- Every poll spends radio time on a device that may be silent; the failure ladder bounds it, and
  `status_poll_interval` lets a user lengthen it.
