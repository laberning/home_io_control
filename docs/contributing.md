# Contributing
<!-- doxygen-label: contributing -->

The most useful contributions to this project are hardware validation and real-world device
reports. If you have an IO-Homecontrol device or ESP32 LoRa board that is not yet covered here,
your testing results are valuable even when the outcome is "does not work yet".


## Hardware and device testing

- Test unvalidated boards, radios, or device families and report whether pairing, commands, and
  status feedback worked.
- Confirm pin mappings for boards that are still marked as untested, or suggest corrected mappings
  when vendor documentation is incomplete or wrong.
- Report negative results too. A failed setup is still useful when it includes the board model,
  radio chip, wiring or pinout, YAML config, and logs.
- If a device only partly works, include what does work and what does not. For example: pairing
  succeeds but status never updates, or open and close work but stop does not.

## Reporting unsupported devices

If pairing discovers a device type that is not yet supported, or the generated YAML snippet is
incomplete, please open a GitHub issue and include enough data to reproduce the problem.

Use this checklist when collecting logs:

1. Enable debug and frame logging in your ESPHome config:

```yaml
esphome:
  build_flags:
    - -DIOHOME_FRAME_LOG

logger:
  level: DEBUG
```

   Do not silence the `home_io_control.frame` log tag (see
   [Tips and tricks](tips-and-tricks.md#quieting-frame-warnings-in-the-log)) in a log you attach to a
   report: its lines show why the hub ignored a frame.

2. Trigger the action that shows the problem.
   For new devices, put the device into pairing mode, press the Discover & Pair button, and capture
   the log from the button press until the pairing flow finishes.

   A normal `-DIOHOME_FRAME_LOG` build never exposes your real system key: key-transfer (0x32)
   payloads are always masked in frame logs (`[N bytes masked]`), and turning frame logging on
   cannot disable that. It is still good practice not to paste pairing logs (commands
   `0x31`/`0x32`/`0x33`) into a public issue unless they are what you are debugging.

   If you would like your log to become a permanent regression fixture rather than a one-off
   report, see the
   [corpus README](https://github.com/laberning/home_io_control/blob/main/tests/corpus/README.md):
   command and status logs can be contributed directly, and pairing logs go through
   `ingest.py --rekey` first. Capturing the raw, unmasked `0x32` bytes for a re-keyed contribution
   needs the opt-in `IOHOME_UNSAFE_LOG_KEY_MATERIAL` build flag; never use it for a bug-report log.
3. Include the board model, radio chip, and full pin mapping you used.
4. Include the device model or product name if you know it, and mention whether it was previously
   paired with another hub.
5. Include any raw values reported by the logs, especially `io_device_id`, `io_device_type`, and
   `io_subtype`, even if they appear as numeric values such as `0x11`.
6. Include the relevant YAML snippet you used for `spi:`, `home_io_control:`, and the affected
   entity if one already exists.

Open issues here: [GitHub Issues](https://github.com/laberning/home_io_control/issues).

## Writing documentation

Everything under `docs/` and the root `README.md` is plain GitHub-flavoured Markdown. The published
site is generated from it by `scripts/stage-docs.py`, which rewrites it into doxygen syntax at build
time — nothing doxygen-specific is ever committed, so what you write is what GitHub renders.

A new page needs two registrations, and no `Doxyfile` edit.

**A page label**, on a line of its own anywhere in the file. It becomes the page's URL on the site
(`my_page.html`), so a page can be moved or renamed without breaking external links. Labels are
unique across the whole site.

```markdown
# My page
<!-- doxygen-label: my_page -->
```

**A parent bullet**, in some other page's subpages block. GitHub renders this as an ordinary bullet
list; the site turns it into a nested tree.

```markdown
<!-- doxygen-subpages -->
- [My page](my-page.md)
<!-- /doxygen-subpages -->
```

Every page needs exactly one parent. `make docs-link-check` fails the build on a page that has
none, since an unparented page is published but unreachable from the sidebar.

Two things that catch people out:

- **Link pages, not repeated headings.** Doxygen numbers duplicate heading anchors site-wide, so a
  link to `other.md#see-also` resolves on GitHub and lands nowhere on the site. Deep links to
  headings that are unique are fine and encouraged — the same check enforces both.
- **Keep example fences flush left.** The tooling only recognises a fenced block when its opening
  and closing fences start at column 0. Indent one inside a list item and the checker reads its
  contents as real markers and links.

Run `make lint` before opening a pull request; it checks links, anchors, labels and parents, and
tells you exactly what is missing.

### Adding a device page

A device gets a page under `docs/devices/` when it is confirmed on hardware and has facts of its own
that do not fit its row in [Supported devices](supported-devices.md): a pairing menu, a quirk, a
working YAML, evidence. A device that is set up exactly like another one stays a row that links to
that device's page.

Give a new page these sections, in this order. Leave out a section that has nothing to say, and add
one of the device's own where it needs it, such as how the device appears on the radio or a check to
make before pairing:

| Section | What goes in it |
|---|---|
| Opening paragraph | What the device is, what the hub does with it, how many units are confirmed on which board, and the issue |
| `## What works` | A table of capabilities with ✅, ⚠️ or ❌ and one line each |
| `## Modes in Home Assistant` | Heaters only: one or two sentences and a link to the device's kind in [Heaters and water heaters in Home Assistant](heating.md). The mode table lives there, once per kind |
| A pairing section | The menu path on the device and what the hub prints; name it after the device ("Pairing a radiator") so other pages can link to it |
| `## Working YAML` | One complete entry that works |
| `## Known quirks` | What surprises an owner, one bullet each, with what to do about it |
| `## Evidence` | Links to the corpus captures and the issue |
| `## See also` | The configuration page and related devices |

Register the page as above: a label `dev_<vendor>_<product>`, and a parent bullet in the "Device
pages" block of [Supported devices](supported-devices.md). Then shorten the device's row there to its
status, at most two sentences and a link to the page.

## Pull requests

Pull requests for fixes, tests, documentation, and targeted improvements are welcome.

For larger features, new platform support, or broader architectural changes, please open an issue
first to check whether the work aligns with the current direction of the project. That helps avoid
spending time on changes that are unlikely to be merged and makes it easier to agree on scope
before implementation.

## See also

- [Development setup](development-setup.md) — the toolchain, test targets and flash commands
- [Supported devices](supported-devices.md) — the matrix your device report feeds
- [Architecture overview](architecture_overview.md) — how the component is put together, and the
  decision records behind it
