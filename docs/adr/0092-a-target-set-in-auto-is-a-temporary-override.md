# ADR 0092: A target set in auto is a temporary override

<!-- doxygen-label: adr0092 -->

**Status:** Accepted · **Recorded:** 2026-11

## Context

A register heater ([ADR 0054](0054-heaters-are-read-back-over-the-private-register-protocol.md)) running
its own weekly programme alternates between a comfort temperature and a lower eco temperature. Home
Assistant shows the programme as `auto`, "the device is set to a schedule, learned behavior, AI"
([climate entity](https://developers.home-assistant.io/docs/core/entity/climate/)).

A target from the card was written to the comfort setpoint (register `0x0103`) in every mode. On an
Atlantic Adelis towel dryer in an eco period that set the comfort setpoint to the typed value, and the
dryer went on heating to that value minus its setback: 17.5 °C typed, 14.0 °C held, and a changed comfort
temperature afterwards ([issue #202](https://github.com/laberning/home_io_control/issues/202)).

The dryer has a register for a temporary override, `0x011D`. The reporter's sweep read 16.5 °C there
while an override set on the dryer's panel ran, and 0 otherwise. The dryer's own panel sets an override
in an eco period, which ends at the next comfort period, and changes the comfort temperature in a comfort
period. The hub followed the panel: an override in an eco period, the comfort setpoint in a comfort
period. A card then does two different things with the same gesture, depending on the time of day.

Home Assistant's integration for the vendor's cloud sends a target as a derogated target whenever the mode
is `auto`, on the towel dryer and on the radiator alike, and shows the effective setpoint as the target in
`auto`.

On the dryer the temperature in force is register `0x0137`. The same sweep read the same temperature in
`0x012A`. No heater with an adjustable setpoint other than the towel dryer (AGILIA IO, Sauter Hekla,
Thermor EMOTION 3 H) has been read for `0x011D` or `0x012A`.

## Decision

- **While the heater's programme runs, a target is a temporary override.** It is written to the override
  register and read back; the comfort setpoint and the programme stay as they are. In `heat` a target is
  the comfort setpoint. Whether a heater has the programme's registers is known only once it has answered
  the read of its temperature in force.
- **"The programme runs" means the heater's mode, as read when the target is set, shows as `auto`** and no
  timed run (boost, drying) is on top: PROG on a towel dryer, PROG and AUTO on a heater with an adjustable
  setpoint. The profile's observed-state table says so; no mode value is named twice.
- **While the programme runs, the card's target is the temperature in force**, where the heater reports
  one.
- **The rule applies to every register heater whose profile names its programme registers**: the towel
  dryer (`0x0137`, `0x011D`) and the heater with an adjustable setpoint (`0x012A`, `0x011D`). The second is
  experimental until a radiator confirms it.
- **A heater that answers the temperature-in-force read without a value keeps the comfort-setpoint
  write.** An empty answer marks the device until the next boot: the read is not repeated, the card's
  target stays the comfort setpoint and a target writes it.
- **A target on a heater with a programme asks the heater what it runs first.** Before it is planned, the
  hub reads the heater's mode and, on a towel dryer whose mode shows its programme, the timed run on top
  of it. They decide which register a target belongs in. The heater can change what it runs between two
  polls (a change on its panel, a boost or drying run ending on its timer), so no stored reading is used
  for this decision, and the mode the card shows is not an input. If the programme runs and the heater has
  not yet answered the read of its temperature in force since boot, the hub reads that too. With a value
  the target is an override, with an empty answer a comfort-setpoint write. A heater that has answered
  with a value keeps the override rule until the next boot, even if a later read is missed or comes back
  without a value.
- **A heater that does not say what it runs gets no target.** If the read of the mode or timed run gets no
  reply or no value, or the read of the temperature in force gets no reply while the programme runs,
  nothing is written and a warning names the device: an override could reach a heater without the
  register, and a comfort write would change the programme. A mode, a preset (a boost among them)
  and the `heating_control` action's functions are sent without such a read.
- **A refused override is logged and nothing else is written.** When the read-back differs from the
  request or carries no value, a warning names the device and the values, and the next poll shows what the
  heater holds. The hub never falls back to the comfort setpoint.
- **An override is ended with the `heating_control` action**, function `set_derogation`, value `0`. It
  also ends by itself, as the heater decides.

## Consequences

- A user who wants a different comfort temperature sets it in `heat`, or on the heater.
- The card behaves the same on every register heater and at every time of day.
- A target costs one to three register reads before its write (the mode, a towel dryer's timed run, and
  the temperature in force on first use) on every register heater with a programme.
- The write is unconfirmed on hardware in a comfort period, and on every radiator. If a heater refuses the
  override in a comfort period, the card falls back to the heater's reading at the next poll, and the rule
  for that heater kind returns to the panel's split (see the first considered option) in a new record.
- Writing 0 to end an override rests on the register reading 0 when none runs; it is unconfirmed.
- The docs and the Doxygen mark the behaviour experimental until an owner confirms it.

## Considered options

- **As the dryer's panel does: an override in an eco period, the comfort setpoint in a comfort period.**
  Rejected as the general rule: the same gesture changes the programme at some hours and not at others,
  and the hub must know the period before it can write. Kept as the fallback for a heater kind that
  refuses the override in a comfort period.
- **Always the comfort setpoint, raised by the setback in an eco period.** Rejected: it moves the
  programme's comfort temperature for a change meant for now.
- **Fall back to the comfort setpoint when the override is refused.** Rejected: it changes the programme
  silently, the defect this record removes.
- **End an override by selecting `auto` again on the card.** Not possible: Home Assistant's climate dialog
  does not send a mode equal to the current one.
- **Number entities for the comfort temperature and the setback.** Left for later: comfort can be set in
  `heat`, and the setback register has not been written.
- **Trust the stored mode and track the hub's own writes.** Rejected: the heater changes its own state
  between polls (its panel, a boost or drying run ending on its timer) and an acknowledgement can be lost,
  so a stored mode can be wrong in ways the hub cannot see. The heater is asked instead.
- **Poll more often instead of reading before a target.** Rejected: any interval leaves a window, and a
  target is rare.
