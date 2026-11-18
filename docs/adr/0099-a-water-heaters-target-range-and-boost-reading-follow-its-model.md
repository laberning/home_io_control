# ADR 0099: A water heater's target range and boost reading follow its model

<!-- doxygen-label: adr0099 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) (the target scale) and
[ADR 0081](0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md) (the state is FP16).
Builds on [ADR 0067](0067-a-model-below-the-subtype-for-products-that-share-a-wire-type.md) (a model
below the subtype).

## Context

An Atlantic Explorer V4 pairs as node type 51, subtype 0, with the name `DHWP Actuator` and the brand
`A`, exactly as the Atlantic Calypso Connecté of ADR 0068 does. It answers the same status read and the
same registers, and takes the same commands. Read with the Calypso's rules, three things come out wrong.
All of it was seen on one heater, with its display read beside the log.

- **The target.** The main parameter is on the same relative scale 0..`0xC800`, but over 20–70 °C
  where the Calypso spans 20–62 °C. The display showed 51 °C at `7C00`, 52 °C at `8092`, 55 °C at `8C00`
  and 59 °C at `9C00`; the Calypso scale reads those as 46.0, 47.0, 49.4 and 52.8 °C. A target written
  on the Calypso scale lands higher on the heater: 47 °C became 52 °C.
- **Boost.** FP16 reads `4900` at rest and `4A00` while a boost runs, whether it was started on the
  display or by the hub. The Calypso reports `4A00` and `4900`. The boost duration register `0x0000`
  reads 0 and 1 in the same states on both heaters, so the state is the same and the field differs.
- **Away.** FP16 stays at `4900` while away. The main parameter drops to `0000`, as the Calypso's does
  during a holiday (ADR 0081), and the away duration register `0x0005` reads `FF`.

The commands of ADR 0081 work unchanged: FP16 `4900` starts a boost, `4600` starts away and `4A00`
ends both.

## Decision

- **The upper end of the target scale is a setting of the device.** `max_temperature:` on the water
  heater says what `0xC800` stands for; the lower end stays 20 °C. The default is 62 °C, so a
  configuration without the key encodes and decodes as before. The range is kept in the device record's
  temperature range, the one a climate device's `min_temperature:` and `max_temperature:` already fill,
  and the card's highest target follows it.
- **The Explorer V4 is a model.** Nothing the heater reports tells it from a Calypso, so the YAML
  declares it: `io_model: explorer_v4` under `domestic_hot_water`, subtype 0. Without the model nothing
  changes.
- **The model reads the boost field the other way round.** Its row carries the capability
  `DHW_BOOST_INVERTED`: boost shows on while FP16 bits 8–9 are `2`, and off while they are `1`.
- **The model makes `max_temperature` default to 70.** The key still wins when both are set.
- **Away is read from FP16 or from the main parameter**, on every water heater: on while the absence
  field is `1`, or while the main parameter is `0000`. It is not tied to the model, because the Calypso
  reports the same main parameter during a holiday and an Explorer V4 declared with `max_temperature`
  alone still shows its away.
- **The writes are those of ADR 0081**, for both heaters.

## Consequences

- A Calypso configuration builds to the same frames and the same readings. The one new case is a heater
  that reports a main parameter of `0000` while its FP16 says away is off: it now reads as away. No
  Calypso capture in the corpus does that.
- A kept target at the very bottom of the scale would read as away, since both are `0000`. Neither heater
  has reported it other than while away.
- An Explorer V4 declared without the model shows a boost while none runs and a target a few degrees
  low. The pairing output cannot warn about it, since the hub cannot tell the two heaters apart; the
  water heater page says how to recognise it.
- While a boost runs the Explorer V4 reports a target of `A800`, 62 °C on its scale, and its own target
  again afterwards. The card shows what the heater reports.
- The heater rounds a target to whole degrees and answers every target EXECUTE with result code 10
  while it applies it. In manual with ECO on it keeps at most 55 °C. The hub shows the target the heater
  reports after the write, as before.
- Another water heater with its own scale needs only `max_temperature`. One with another difference gets
  a model row.

## Considered options

- **A fixed 70 °C for the model and no key.** Rejected: one heater is known, and whether other products
  share its scale is not. The key costs one setter and lets an owner correct the scale without a new
  model.
- **Telling the heaters apart from what they report.** Not possible with what has been read: type,
  subtype, name and brand are the same.
- **Reading boost from register `0x0000`**, which reads the same on both heaters. Rejected: it is a
  duration, and it is read in a later exchange than the status reply the switch follows after a write
  (ADR 0081).
- **Tying the main-parameter reading of away to the model.** Rejected: the Calypso reports the same
  value, and one rule for both heaters is simpler than a second capability.
