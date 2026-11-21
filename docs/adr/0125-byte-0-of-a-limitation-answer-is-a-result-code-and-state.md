# ADR 0125: Byte 0 of a limitation answer is a result code and state

<!-- doxygen-label: adr0125 -->

**Status:** Accepted, amends [ADR 0050](0050-rain-is-read-by-an-opt-in-limitation-poll.md) · **Recorded:** 2026-11

## Context

The rain poll asks a device for the minimum limitation of its main parameter (`0x25`) and reads rain from the answer
(`0x26`). The decoder took byte 0 of the answer as the parameter id and gave no rain value unless it was 0, the main
parameter. A VELUX window answers with 0 in byte 0. A Somfy Sunea awning answers with `05`, so its rain reading was
always unknown.

The corpus shows that byte 0 is not a parameter id:

- The request already names the parameter (`25 <80|C0> 00 00`, the main parameter in data[1]). The awning answers `05`
  to the minimum and the maximum alike, at 97 %, fully open and fully closed
  (`tests/corpus/captures/probe/somfy_awning_probe_limitation_sx1262.yaml`). As a parameter id, `05` would name a
  parameter nobody asked for.
- The awning puts the same `05` in byte 0 of its plain status answers at rest (`0x04`), of its short-form parameter
  answers that carry a value (`0x0D`) and of all six limitation answers (`0x26`). Its answers to a move command and its
  status answers in motion carry `04` or `2C` (low three bits 4 instead of 5), so the byte follows the motor's state,
  not the request. The VELUX window answers `0x26` with `00`
  (`tests/corpus/captures/probe/velux_window_probe_limitation_rain_sx1262.yaml`).
- Read as the status byte that the hub already splits for `0xFE` and the EXECUTE answer (result code = byte >> 3,
  execution state = byte & 7), the code is 0 in every `0x26` answer in the corpus. In the `0x0D` answers of the
  awning, two Izymo dimmers and the VELUX window, the code is non-zero (`6D` on the Somfy devices, `68` on the window)
  in every answer that carries only the unknown-position bytes `D4 00` and no value, and 0 in every answer that carries
  one. The one exception is a short-form `0x0D` answer from an unrelated session whose request was not captured: it
  reads `05 D4 00 00 00`, code 0 without a value. The code therefore reads as a result code in read answers from both
  vendors.
- A non-zero code does not always mean an empty answer: the awning answers the extended private request
  (`03 <fn> 80 <block> 00`) at rest with `2D` (code 5, state 5) and data after it
  (`tests/corpus/captures/probe/somfy_awning_probe_extended_fn_sx1262.yaml`). That answer is not a read of a
  parameter, and this record covers only the `0x26` answer.

## Decision

- Byte 0 of a `0x26` answer is decoded as the status byte: result code and execution state. The raw byte stays in
  the debug line.
- A limitation answer gives a rain value only when its result code is 0, whatever the state bits. A non-zero code
  gives no value and is logged with its name.

## Consequences

- A Somfy awning that answers `05 00 00 00 00` to the minimum-limitation request reads dry instead of unknown.
- The VELUX window's dry and wet answers read as before.
- No capture shows a Somfy device under a rain or wind lock, so a wet reading from a Somfy device is not confirmed.
  The rain result of a device other than VELUX is marked experimental in the docs until such a capture exists.
- Nothing changes on the air; only the decoding changes.
