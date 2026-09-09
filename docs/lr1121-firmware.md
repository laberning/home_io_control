# LR1121 firmware update
<!-- doxygen-label: lr1121_firmware -->

> **⚠️ Give this the same care as any firmware update.** Make sure the device is on stable mains
> power that nobody is going to unplug, and don't power-cycle it while a flash is running. Most of
> what's described here is recoverable if it goes wrong; one part of it is not, and that part is
> clearly marked.

## What's actually on the radio chip

The LR1121 is a separate chip from the ESP32, and it runs its own software — nothing to do with
this ESPHome component or the firmware you flash over USB/OTA. That software comes in two pieces,
and it helps to know which is which before you update anything:

- **Transceiver firmware** — the radio program itself. This is what talks io-homecontrol. Semtech
  versions it `1.1`, `1.2`, `1.3`, `1.4`, … and publishes each release as a `.bin` file.
- **Bootloader** — a much smaller program that runs first, checks the transceiver firmware is valid,
  and starts it. Crucially, the bootloader is *also* what makes replacing the transceiver firmware
  possible at all. It has its own version numbers: `0x2100`, `0x2101`.

Both come from [Semtech's image repository](https://github.com/Lora-net/radio_firmware_images/tree/master/lr1121),
where the changelog also lives, and this component downloads whichever you point it at when you
build; nothing is bundled here. Semtech's own update tool is
[Lora-net/SWTL001](https://github.com/Lora-net/SWTL001), but you do not need it.

**The two are linked:** a newer transceiver firmware can require a newer bootloader underneath it.
Today that matters in exactly one place — firmware `1.4` requires bootloader `0x2101`, while `1.1`
through `1.3` run on `0x2100`. Most boards shipped with `0x2100`, so reaching `1.4` means updating
the bootloader first. Your device reports both versions in its startup log, so you never have to
guess which situation you're in.

## How risky is each one?

| Updating | Risk | If it fails |
|---|---|---|
| **Transceiver firmware** | Low | Recoverable. The bootloader is untouched, so the chip can always be reached again — press the button once more and re-flash. Worst case the radio is silent until you do. |
| **Bootloader** | **High — not reversible** | The bootloader is what makes recovery possible. If a bootloader write is interrupted, there is nothing left to recover *with*, and this project cannot fix the chip. |

So: updating transceiver firmware is routine and safe to try. Updating the bootloader is a
deliberate, one-way operation that you opt into separately, and it is gated behind its own switch so
you cannot trigger it by accident. Both are described below.

Whichever you do, the single most useful precaution is boring: **stable power throughout.** A flash
blocks the device for a few seconds and it will go briefly unavailable in Home Assistant — that's
normal, and reaching for the plug at that moment is the one reliable way to turn a safe operation
into a broken chip.

Everything on this page has been run end to end on real hardware (a LilyGO T3-S3), including the
bootloader update.

## Flashing transceiver firmware

Triggered from a Home Assistant button, straight from your existing build — no extra hardware, no
vendor tooling.

<!-- board-pinout: t3s3 -->
```yaml
home_io_control:
  radio_type: lr1121   # required
  busy_pin: 34          # required
  lr1121_firmware_update:
    source: github://Lora-net/radio_firmware_images/lr1121/transceiver/lr1121_transceiver_0103.bin
    # ref: optional branch/tag/commit; defaults to HEAD
    # checksum_md5: optional 32-hex-char hash; also the fallback when no `.bin.md5` sibling exists
    # target_version: optional hex (e.g. 0x0103); only needed when the filename carries no version
```

Configuration variables (all nested under `lr1121_firmware_update:`):

- `source` (Required): A `github://<owner>/<repo>/<path/to/file.bin>[@ref]` shorthand pointing at
  the **`.bin` file** itself. Fetched and MD5-verified once, at compile time — not a live
  "check for updates" mechanism, and no firmware binary is ever committed to this repo.
- `ref` (Optional): Branch, tag, or commit to fetch from. Defaults to `HEAD`, or an `@ref` embedded
  directly in `source`.
- `checksum_md5` (Optional): A 32-hex-character MD5 hash to verify the download against, used when
  no `<source>.md5` sidecar exists (or must agree with it, if one does).
- `target_version` (Optional): Overrides the firmware version this build believes it is flashing.
  Normally derived from the filename (`..._0104.bin` → `1.4`); only needed when a mirrored or
  renamed image's filename carries no version.

The block's mere presence is the build flag: adding it recompiles the flash button and a boot-time
bootloader-version read into the firmware; removing it takes them back out. There is no runtime
toggle — entering and leaving flash mode is a recompile + OTA each way. All of your normal
cover/light/switch/lock entities keep working throughout, unaffected.

## Which images can I flash?

Without the `bootloader:` sub-block below, the answer depends on the bootloader your board already
has. On the common `0x2100` that means firmware `1.1` through `1.3`; `1.4` needs `0x2101` and stays
out of reach until you deliberately opt into the bootloader update.

You don't have to look this up. Every boot reads the bootloader version — a free chip reset, before
anything else is configured — and prints it alongside a verdict for whatever you configured:

```
LR1121 bootloader version: 0x2100
Firmware update target: 1.4 -- CANNOT PROCEED: needs bootloader 0x2101, this chip has 0x2100 -- add a bootloader: sub-block to lr1121_firmware_update: to enable the (irreversible) upgrade path
```

That verdict is computed once at startup, so pressing the button when it can't succeed costs
nothing — it refuses immediately without touching the radio.

The boot log also compares the running firmware with the newest version this build knows about and
adds a line pointing here when it is older. That "known latest" is baked in at build time, not
looked up live.

If you point `source:` at a firmware version newer than this component knows about, it is **not**
refused: this build's compatibility table is a point-in-time snapshot, not an authority, so an
unfamiliar version is routed through the two-press confirmation instead. The feature keeps working
on releases that didn't exist when it was written.

## Updating the bootloader (irreversible — opt in deliberately)

> **⚠️ This one is a one-way door.** A failed bootloader write has **no recovery path in this
> project** — see the risk table above for why. Mains power only, and don't interrupt it. Read
> [ADR 0021](adr/0021-flash-the-lr1121-bootloader-behind-an-arming-switch.md)
> before configuring this.

**Why you might want to.** Semtech's advisory SEM-PSA-2026-001 lists three CVEs affecting the
LR1121, and names bootloader `0x2101` as a threshold alongside firmware `1.4`. The most severe of
them is a weakness in secure boot — which is the bootloader's own job — so updating only the
transceiver firmware most likely does not close it. Worth knowing before you rush: all three
require **physical access to the chip's SPI pins**, so this is defence in depth for a hub sitting
inside your house, not an emergency.

**What actually happens.** It's three steps, not one, and the radio has no working firmware in
between: a small *loader* image is written first, that loader rewrites the bootloader, and then your
chosen transceiver image is written on top. The middle step is the one that cannot be retried. The
whole sequence takes about 10 seconds, during which the device is unresponsive.

Opt in with a nested `bootloader:` block, pointing at the matching **loader** image (not a
transceiver image) Semtech publishes alongside the transceiver firmware:

<!-- board-pinout: t3s3 -->
```yaml
home_io_control:
  radio_type: lr1121   # required
  busy_pin: 34          # required
  lr1121_firmware_update:
    source: github://Lora-net/radio_firmware_images/lr1121/transceiver/lr1121_transceiver_0104.bin
    bootloader:
      source: github://Lora-net/radio_firmware_images/lr1121/loader/lr1121_loader_2100.bin
      # ref / checksum_md5: same meaning as the outer block's
```

Pairing this block with an outer `source:` that's known to require the *existing* bootloader
`0x2100` is a build-time error, not merely discouraged: after the rewrite that image would be
unflashable, so ESPHome refuses to compile the config at all rather than let it arm a trap. This
guarantees the recovery transceiver image is already in ESP32 flash before anything is erased, so a
config mistake fails the build rather than bricking a board.

If the outer `source:` names a firmware version this build's compatibility table has never heard of
(the table's advisory, not exhaustive — see the "Which images can I flash?" section above),
that's only a build-time **warning**, not an error: the build will not gamble an irreversible write
on a requirement it doesn't know, so the `bootloader:` block is accepted but the rewrite path stays
inert at runtime (the boot-time log explains why, and the flash button refuses without touching the
chip). Extending the compatibility table once Semtech publishes the pairing is a one-line edit — see
`LR1121_KNOWN_BOOTLOADER_REQUIREMENTS` in `lr1121_firmware_decisions.h`.

Configuring the block also creates an **"Allow LR1121 Bootloader Rewrite (Irreversible)"** switch
on the hub, defaulting **off** and never auto-arming after a reboot. The existing flash button
performs the three-stage rewrite only while that switch is on **and** the cached verdict says the
upgrade applies; with the switch off, a press refuses immediately without touching the chip. There
is no separate two-press confirmation for this path — the switch **is** the confirmation, and it's
visible in Home Assistant so "is this armed?" is answerable by looking rather than remembering.

A completed (or failed) rewrite always ends in an ESP32 reboot, which clears the switch again — the
window is armed-until-next-flash, not indefinitely armed. Once the bootloader is `0x2101`, the
ordinary transceiver path above becomes the recovery mechanism for any future re-flash: the
`bootloader:` block goes inert (a same-bootloader chip has nothing left to upgrade) and every press
uses the plain flash sequence.

**What a successful run looks like.** Turn the switch on, press the button once, then leave it
alone. Expect roughly:

```
LR1121 bootloader rewrite: arming switch is on -- running the three-stage sequence now.
LR1121 bootloader rewrite: Stage 1a (loader write): erasing radio flash, this takes a few seconds...
LR1121 bootloader rewrite: Stage 1a (loader write): erase took 2474 ms, write took 584 ms
LR1121 bootloader rewrite: Stage 1b checkpoint passed -- chip in transceiver mode: type=0xDE fw=33.0
LR1121 bootloader rewrite: Stage 2 -- rewriting the bootloader now. This step cannot be undone.
LR1121 bootloader rewrite: Stage 2 chip accepted UpdateBootloader (command_status=2)
LR1121 bootloader rewrite: Stage 2 complete -- bootloader is now 0x2101.
LR1121 bootloader rewrite: Stage 3 (transceiver write): erase took 2474 ms, write took 1945 ms
LR1121 firmware update: success -- now running 1.4
```

The **Stage 1b checkpoint** line is the meaningful one to watch for: everything up to that point is
still fully recoverable, and it is what confirms the loader really is running before the
irreversible step begins. After it, the device goes quiet for a few seconds — that is the flash, not
a crash. Once it reboots, the startup log should report bootloader `0x2101` and firmware `1.4`.

If anything goes wrong the log says which stage failed and whether it is recoverable, in plain
terms, rather than leaving you to work it out. A stage 3 failure in particular is *not* serious: the
bootloader is already updated, and the ordinary flash button (switch off) finishes the job.

## The flash button and two-press confirmation

Configuring the block creates a **"Flash LR1121 Radio Firmware"** button, bound directly to the
hub. Pressing it:

- **Refuses immediately, without touching the chip**, if the configured target can't proceed
  (wrong bootloader) or the radio doesn't look like an LR1121 at all — the reason is the same one
  already printed in the boot-time config dump.
- **Asks for a second press** if the target isn't unambiguously newer than what's installed (already
  installed, unverified compatibility, or an unknown version). A second press within about a minute
  proceeds; letting the window expire cancels it.
- **Flashes immediately** if the target is a known-good, strictly newer version.

Once a flash starts, the chip is erased and rewritten, with progress logged periodically
(`Flashing LR1121: 1630/16304 words (10%)`) on both the serial console and the API connection — on
a `1.3` image this takes a few seconds total. The ESP32 reboots on its own once the sequence
finishes, including after a failure. **A failed erase or write is recoverable**: press the button
again after the reboot to retry.


## See also

- [Hardware](hardware.md) — which boards carry an LR1121
