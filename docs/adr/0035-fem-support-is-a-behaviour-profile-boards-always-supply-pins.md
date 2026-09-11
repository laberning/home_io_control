# ADR 0035: FEM support is a behaviour profile; boards always supply their own pins
<!-- doxygen-label: adr0035 -->

## Status

Accepted. Implemented in the SX1262 driver (`radio_sx1262.h`/`.cpp`) and the `fem:` config key
(`components/home_io_control/__init__.py`) for all three profiles, and in the `heltec-v4-2` /
`heltec-v4-3` board packages. The `t-beam-1w` board package (the `xy16p35` profile's own
board) ships separately once its TCXO control voltage is confirmed — see
[Hardware](../hardware.md#front-end-module-fem-support) — the driver and schema support for
`xy16p35` do not depend on that answer and are not blocked by it. None of the three profiles
has been run against real IO-Homecontrol hardware yet.

## Context

Some SX1262 boards front the chip with an RF front-end (FEM) — a PA and LNA path between the
SX1262 and the antenna, controlled by up to three GPIOs: a power enable (`vfem_pin`), an
optional secondary enable (`fem_en_pin`, called CSD on the parts that have one), and a "mode
pin" (`fem_pa_pin`) selecting the TX/full-PA path from the RX/LNA path. Three such front-end
parts are known to this project — GC1109 and KCT8103L (Heltec WiFi LoRa 32 V4.2/V4.3) and
XY16P35 (LilyGO's T-Beam 1W SX1262, 868 MHz variant) — sharing that control vocabulary, though
XY16P35 has no secondary enable pin at all. GC1109 and KCT8103L are each a single FEM IC;
XY16P35 is an RF module (SX1262 + PA + antenna switch + TCXO as one sealed sub-assembly) with no
individually-identifiable "FEM chip" inside it in anything published — a genuine difference in
kind from the other two, not just a naming convenience. GC1109 and KCT8103L share one control
polarity on their mode pin; XY16P35 uses the inverse one (its "LNA Ctrl" pin is active-HIGH on
*receive*, not transmit — LilyGO's own datasheet).

GC1109 and KCT8103L's shared control-logic table (CSD/CTX/CPS):

| Mode | CSD | CTX | CPS |
|---|---|---|---|
| Shutdown | 0 | X | X |
| Receive, LNA | 1 | **0** | X |
| Transmit bypass (~1 dB insertion loss, PA bypassed) | 1 | 1 | 0 |
| Transmit, full PA | 1 | 1 | **1** |

On Heltec V4.2, CPS is a plain GPIO (the mode pin) and CTX is the SX1262's own DIO2, which
`SetDio2AsRfSwitchCtrl` already drives HIGH-during-TX/LOW-otherwise automatically — so DIO2
already does the right thing to CTX with no driver involvement. On V4.3 the wiring is swapped:
CPS is DIO2 (automatic, and don't-care whenever CTX is 0) and CTX is the plain-GPIO mode pin —
so the pin the driver must actively drive per transmission is CPS on V4.2 and CTX on V4.3,
different signals, same physical role.

No single static level is correct for this role in both directions: on V4.3, leaving the mode
pin LOW means CTX rests at 0 and the board cannot transmit at all (the FEM's internal switch
routes ANT to the LNA input, never the antenna); strapping it HIGH gets TX working but leaves RX
running through the ~1 dB passive bypass path instead of the +21 dB LNA, a large silent
sensitivity loss. The driver needs to know when a transmission starts and ends, on every
front-end part whose mode pin lacks a universally-correct static level — which, on the evidence
so far, is every one this project has looked at.

## Options considered

Two questions, weighed together: where does a board's FEM control *behaviour* live, and where
do its *pin numbers* come from.

1. **Each board package encodes its own FEM's control logic directly**, with no shared enum or
   driver hook. Rejected: GC1109 and KCT8103L's control logic is bit-for-bit identical, so this
   duplicates the one place a polarity bug could hide, for zero benefit — and a board package is
   the wrong layer to own chip-level RF-switch timing in the first place, the same reasoning that
   already keeps every other radio behaviour out of `config/boards/*.yaml`.
2. **A `FemProfile` selects control behaviour, and also supplies each profile's default GPIO
   numbers** whenever a board's config omits them. Rejected: every real board package still has
   to spell its pins out explicitly regardless, so `scripts/check-board-pinouts.py` can
   cross-check them against the docs — the same standard every other radio pin in this schema
   already meets — which makes profile-supplied pin defaults dead weight the moment a board
   ships. The approach also has no clean way to express "this part has no `fem_en_pin`-equivalent
   at all" (XY16P35) without an awkward per-profile carve-out in whatever supplies the defaults.
3. **A `FemProfile` selects control behaviour only; every board package supplies every pin its
   profile needs, validated at config time.** No defaulting, no guessing. A missing required pin
   is a build-time `cv.Invalid` naming exactly which key is absent, not a silent no-op discovered
   on the bench.
4. **Runtime auto-detection** — power `vfem_pin`, read a chip-identifying pull direction as an
   input, infer which profile applies. Rejected: it needs the YAML to declare every *candidate*
   mode pin up front so the driver can pick one at boot, a worse config surface than the thing it
   removes, and it puts board-identification logic inside a radio driver instead of a config key.
   A pull-direction difference (where one exists) is still worth documenting as the *user-facing*
   way to tell two similar boards apart with a multimeter — it just doesn't belong in firmware.

## Decision

Option 3. `fem:` (`none` / `gc1109` / `kct8103l` / `xy16p35`) selects a `FemProfile` that
controls exactly two things: which level `fem_pa_pin` is driven to for the duration of a
transmission (`fem_tx_active_level_()` — `true`/HIGH for GC1109 and KCT8103L; `false`/LOW for
XY16P35, per its inverted "LNA Ctrl" sense above), and which of `vfem_pin`/`fem_en_pin`/
`fem_pa_pin` a schema-time validator requires present for that profile
(`FEM_REQUIRED_PINS`). It never supplies a GPIO number. The one place a real per-part
difference exists — XY16P35's inverted polarity — gets exactly one boolean flip, not a
structural branch; GC1109 and KCT8103L share one code path with nothing distinguishing them but
their TX-gain table.

The behaviour is wired into the four seams that already exist between `RadioSX1262` and the
shared RX/TX orchestration in `SoftPhyDriverBase`: `before_tx_arm()` drives the mode pin to its
TX-active level immediately before `SetTx`; `set_mode_rx()` drives it to the opposite level
before re-entering continuous receive; `set_mode_standby()` drives it to that same opposite level
too — not just `set_mode_rx()` — because `SoftPhyDriverBase`'s two TX-timeout paths call
`set_mode_standby()` directly and never reach `set_mode_rx()` afterward, and without this second
hook a timed-out TX would leave a board stuck in its TX-active FEM state indefinitely, with no
automatic recovery. `FemProfile::NONE` is a no-op fast path on both hooks, so a bare SX1262 board
with no FEM chip at all, or a raw `fem_en_pin`/`vfem_pin`/`fem_pa_pin` config with no `fem:` set,
keeps exactly the pre-existing strap-HIGH-once-and-never-touch-again behaviour those configs
already had — this is the population every `NONE` guard protects, not any one specific board.

Init-time pin sequencing raises `vfem_pin`, waits briefly, then raises `fem_en_pin` (on the parts
that have one), then sets the mode pin to its receive/idle level for a configured profile — the
power-on order GC1109 and KCT8103L's datasheets require (supply before any control pin is
driven); XY16P35 has no published datasheet to state the same requirement, but the same order is
applied uniformly regardless, since it is harmless where it isn't strictly needed. `vfem_pin`'s
electrical scope is not identical across parts: on the Heltec boards it gates only the FEM's own
supply (the SX1262 itself runs ungated off `VDD_3V3`); on the T-Beam 1W it gates the entire
SX1262+PA module's power. The driver does not currently distinguish these cases in its settle
timing — an open risk, not yet hardware-tested on the T-Beam 1W.

A missing required pin fails config validation immediately, naming which key is absent and which
profile required it, rather than constructing a driver with a null `fem_pa_pin_` that silently
never toggles anything.

**TX power is not radiated power on an FEM-equipped board.** On the two Heltec chips, a
mandatory input-protection pad sits ahead of the PA even in bypass mode (the GC1109's absolute
maximum input at its TX port is +5 dBm), so the PA is not optional — it exists to overcome the
very pad that protects it, and running it at several dB of back-off from `tx_power`'s low end is
both legal and its most linear operating region. Net gain through the whole chain is estimated at
roughly +11 dB (GC1109) / +13 dB (KCT8103L) at low drive from three sources that disagree with
each other by several dB, and roughly +14 dB (XY16P35) from LilyGO's own five measured points
— a sparser but more directly-measured evidence base than the other two. Each board package ships
the `tx_power` setting that keeps its own profile's estimate at or under the 868 MHz SRD ERP
limit (+14 dBm): `1` for both Heltec chips, `0` for XY16P35. The driver logs its own
antenna-port estimate (with that uncertainty attached) at boot, and the schema separately warns
whenever a `fem:` profile is active and `tx_power` is set above that profile's own quiet ceiling.

`Vfem` and `Vext` are electrically unrelated on both Heltec V4.2 and V4.3 schematics — two
independent LDOs off the same 5 V rail, with `Vext` (GPIO36) powering only the OLED and expansion
header, never the FEM. No GPIO36 handling exists anywhere in this feature.

## Consequences

- Adding a fourth FEM profile is: one `FemProfile` enumerator, one entry each in
  `FEM_REQUIRED_PINS`/`FEM_PROFILES`/`FEM_TX_POWER_MAX_QUIET`, a `-Werror=switch`-enforced entry
  in the TX-gain table dispatch, and — only if its control polarity genuinely differs from both
  existing ones — one more branch in `fem_tx_active_level_()`. No existing board package needs to
  change, and no existing board package's behaviour changes.
- Every FEM-equipped board package must spell out every pin its profile needs, in full, with no
  implicit default anywhere. This is more verbose per board than a design that supplies defaults
  would be, and deliberate: it is the same standard every other radio pin in this schema already
  meets, and it is what makes `scripts/check-board-pinouts.py`'s cross-check meaningful instead
  of decorative.
- `xy16p35`'s per-`tx_power` antenna-port estimate rests on five sparse vendor-measured points,
  not a 22-point table like the other two profiles; the boot-time log and the docs say so rather
  than implying the same confidence.
- `vfem_pin` gates a different scope of hardware on the T-Beam 1W than on the Heltec boards
  (whole module vs. FEM-only), and the driver's init-time settle delay does not yet account for
  that difference — flagged, not resolved; the first T-Beam 1W bring-up should watch for a BUSY
  timeout here specifically before assuming an unrelated cause.
- Nothing here is hardware-validated on any of the three boards. The first real bench report
  against any of them — GC1109, KCT8103L, or XY16P35 — should carry a conducted-power
  measurement.
