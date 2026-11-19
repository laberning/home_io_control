# ADR 0099: An electric storage water heater is a model read from its registers

<!-- doxygen-label: adr0099 -->

**Status:** Accepted · **Recorded:** 2026-11

Extends [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md) and
[ADR 0081](0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md), uses the model rows of
[ADR 0067](0067-a-model-below-the-subtype-for-products-that-share-a-wire-type.md), and picks its modes by what heats,
as [ADR 0090](0090-a-water-heaters-modes-are-chosen-by-what-heats.md) does.

## Context

A Sauter Guelma connecté 80 L reports the node type of the Atlantic Calypso (51/0) but is another product. Its status
reply carries a main parameter of 0 and FP15 = FP16 = `FFFF` in every display state, and registers `0x0000`, `0x0005`
and `0x0021` answer with no value. Read as a Calypso, it showed a 20 °C target and every capability, and it
acknowledged an away command that changed nothing.

Register sweeps in each display state show what it holds instead: the temperature it heats to (`0x0049`), the showers
it aims for, has and is set to (`0x004A`, `0x0046`, `0x0048`), its heating power (`0x0032`), and one flag each for a
running boost and an absence (`0x0052`, `0x0053`). Nothing the hub reads before the first poll tells the two heaters
apart, and the entity's modes and range are fixed at setup.

## Options considered

- **Detect the model at runtime from `FFFF` or the model name.** Rejected as the selector: the traits are fixed at
  setup, as ADR 0067 found for the towel dryer. The `FFFF` answer stays useful as a warning.
- **Read boost and away from the state register `0x0035`.** Rejected: it tells the four states apart, but after a write
  it still held the old state for some seconds while the flags already held the new one.
- **A model row with its own poll and writes** (chosen).

## Decision

1. **`io_model: electric_water_heater`** is a model row below 51/0. `dhw_state_in_registers()` is the one test the
   controller and the entity use.
2. **Its poll reads registers only**, with no status read and none of the three registers it leaves empty.
3. **Boost and away are a write of their flag**, read back at once; the entity shows the read-back, never the command.
   A flag the heater does not keep fails the write.
4. **The mode write stays register `0x0006`.** The entity offers `eco` (automatic, as on the Calypso) and `electric`
   (manual: only the element heats).
5. **A target selects a shower count.** The heater takes no free target: 2, 3 and 4 showers heat to 50, 60 and 70 °C,
   so a target in manual mode writes the nearest count to `0x0048`. In automatic mode and in absence it is dropped.
6. **A heater declared without the model that answers `FFFF`** is named in a warning, its capabilities read as none,
   and its main parameter is not taken as a target.

## Consequences

- The Calypso path is unchanged: every difference hangs on the declared model.
- **The user must declare the model.** The block that discovery prints names the wire type only, so a Guelma runs as
  the other heater until the warning is acted on.
- All of this rests on one 80 L unit. Other capacities may offer other shower ranges, and the temperature of each
  count is a fact of this unit.
- What selects the 2250 W heating level is not known; the sensor reports it without explaining it.
