# ADR 0106: A water heater's values are readings of the same table

<!-- doxygen-label: adr0106 -->

**Status:** Accepted, amends [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md), [ADR 0103](0103-every-heater-value-is-a-reading-kept-by-reading.md), [ADR 0104](0104-a-heaters-poll-reads-what-its-reading-rows-select.md), [ADR 0105](0105-every-device-value-write-is-judged-by-one-rule-per-register.md) and [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) · **Recorded:** 2026-11

Builds on [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) (a domestic hot water device is a water
heater), [ADR 0081](0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md) (boost and away travel in
the operating-mode word) and [ADR 0105](0105-every-device-value-write-is-judged-by-one-rule-per-register.md) (one write
rule per register).

## Context

The reading table of [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md)
covered heaters only. A water heater kept its own decoders, its own map from register to stored field, its own poll
list, its own entity classes and its own encoders, although it answers the same private register protocol. Its
registers sit in another register family (high byte 0x00 instead of 0x01) and hold their multi-byte values most
significant byte first; one register (what is heating) carries several independent facts in different bytes; and the
boost, away and capability values come as functional parameters of the status reply, not as registers
([issue #198](https://github.com/laberning/home_io_control/issues/198)).

## Decision

1. **One register table for every family.** A register's row names its family (the high byte) next to its low byte.
   The table and its types have device-neutral names (`DeviceRegister`, `DEVICE_REGISTERS`). Byte order is the codec's,
   so the family carries no byte-order flag. One check decides whether a device takes a register: a family-0x00
   register goes only to a water heater, a family-0x01 register only to a heater read over private registers.
2. **Shared codec forms for the water heater's values:** signed 16-bit big-endian tenths, unsigned 32-bit big-endian
   thousandths (an energy counter in Wh shown in kWh), and a flag that is on when any byte of a slice is non-zero.
3. **A reading can be part of a register.** A register source may name a slice of the value and the codec for it; a
   register reply stores every reading of that register. What is heating becomes one reading per fact (heating, heat
   pump, electric backup), each a slice of the same register, read once.
4. **Status functional parameters are a source kind.** A reading can be a functional parameter of a status reply,
   either the whole 16-bit word or a two-bit field that reads 1. One decoder walks the functional parameters a status
   reply carries; the hub's status decoder stores these readings as it stores the main parameter.
5. **One status setpoint reading.** The setpoint a status reply reports is one reading for every device, decoded over
   the device's encoding range (the configured range, a profile's fixed range, or the water heater's 20–62 °C) and
   rounded to tenths of a degree.
6. **A device without a heating profile is polled by its rows' capability.** The poll's register list comes from the
   rows whose capability the device has, in table order, under the same poll policies as a heater's.
7. **Which platform offers a reading's YAML key is stated on its mirror entry**, not derived from the source kind.

## Consequences

- A water heater value is added like a heater value: a register row, a reading row and its mirror entry.
- The water heater's energy, heat pump and electric backup entities are the generic reading entities; the water heater
  entity, the heating-source text sensor and the boost switch read through the readings.
- The codec of its own that [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) gave the water heater
  is gone: `proto_dhw.h` keeps the request builders, the mode and operating-word values and the names.
- Mode and duration writes go through the shared encoder and write rules; the target follows the main-parameter rule
  of [ADR 0105](0105-every-device-value-write-is-judged-by-one-rule-per-register.md).
- The boost and away words (FP16) and the capability word (FP15) are stored independently: a status reply that carries
  one of them stores that one and leaves the other as it was, so boost and away can show while the capabilities stay
  unknown.
- The water heater's energy sensor has no state until its first read, which the poll makes on its first run.
- A heater's status setpoint is rounded to tenths and kept inside the device's range: one step of the main-parameter scale is under a thousandth of a
  degree, so 20 °C could read 19.9998 before.
- YAML keys, entities, their metadata and the frames sent are unchanged.

## Considered options

- **A source kind of its own for the water heater's registers:** rejected, the request and reply layout is the same
  as a heater's; only the family and the byte order differ, and the codec already states the byte order.
- **One flag per bit of the heating register:** rejected, the observed values only show which bytes change, not what
  a bit inside a byte means.
- **A second target reading for the water heater:** rejected, it would differ from the heater's only by its range and
  rounding, which the device's range and one rounding rule cover.
