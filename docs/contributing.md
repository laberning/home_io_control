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

## Adding a device reading

A reading is one value a device reports that Home Assistant shows as a sensor: an energy counter, an
occupancy flag, the minutes left of a run. The readings live in one table, so a new one is table rows
and, at most, one new decoder, never a new entity or new YAML code. The pieces are listed in the order
you meet them. The structure behind them is described in the
[Architecture overview](architecture_overview.md), the decisions in
[ADR 0099](adr/0099-scheduled-transfer-reads-use-one-session-and-a-queued-reader.md),
[ADR 0100](adr/0100-a-transfer-read-tells-an-accepted-request-without-an-answer-from-silence.md),
[ADR 0101](adr/0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md) and
[ADR 0102](adr/0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md).

1. **Find where the value lives.** A heater reports it as a private register; some devices keep it in
   a data object. Read the objects with the [`read_object` probe](diagnostic-probes.md#reading-a-device-object).
   `read_object <index>:<offset>` reads an object from an offset, and `probe_sweep` walks the offsets
   of one object between two ends. Note the bytes of the value and what the device sends when it has
   none, and capture the exchange for the corpus (step 6).
2. **Add a transfer recipe** (data objects only). A row of `TRANSFER_RECIPES` in `proto_constants.h`,
   behind the probe's rows, lists the opens that lead to the object, the offsets a read may open at
   and the most bytes accepted. Only what a recipe names can be opened, and nothing builds a write.
3. **Add the reading row.** An enumerator of `DeviceReading` and a row of the table in
   `proto_device_readings.cpp`: its name, its source, when the hub reads it (`ReadingPoll`) and, for a
   reading an entity of the YAML asks for, the capability a device needs (a `DeviceFeature`). The value
   is kept in the device's reading store by reading, whichever the source is, so nothing else holds it.
   For a data object the source is a `DataObjectSource`: the recipe, the offset, the byte range of the
   value in the data read and the codec. Value shapes that exist are in `proto_value_codecs.h`;
   a counter that starts with a marker byte (`80` then a big-endian number, `81 FF FF` for "no value")
   is `MARKED_U16_BE`. A new shape is one codec enumerator, one case in `decode_value()` and its test
   in `tests/proto/value_codecs_test.cpp`.
4. **Add the mirror row.** The same reading by name in `DEVICE_READINGS` in `hub_validators.py`: its
   capability (`None` for a reading no key declares), its source (`data_object`), the YAML key, the entity kind, the Home Assistant unit,
   device class and state class, and for a data object the default `update_interval_ms`, at least five
   minutes. Add the capability's hint to `READING_FEATURE_HINTS` so the error for a kind without it
   says what to declare. `tests/sync/device_features_sync_test.cpp` fails until both tables agree.
5. **Give the device kind the capability.** Add the `DeviceFeature` enumerator the reading names (when
   it is new), put it on the rows of the type table (`proto_device_types.cpp`) that have it and add its
   name to `FEATURES` in `hub_validators.py`. A kind that answers no status read also carries
   `NO_STATUS_READ`, so the hub never polls it for one. A kind that no table row names yet gets its
   row, and its row in the node type table of [Supported devices](supported-devices.md), which
   `tests/sync/docs_node_type_table_sync_test.cpp` checks.
6. **Add the corpus capture.** Frames of the exchange that carries the value, with a provenance note,
   go under `tests/corpus/` as described in the
   [corpus README](https://github.com/laberning/home_io_control/blob/main/tests/corpus/README.md). A
   test in `tests/proto/` decodes them with the new row.
7. **Write the docs.** A row for the device in [Supported devices](supported-devices.md), marked
   experimental until a capture or a reporter's hardware confirms the reading, and the reading key on
   the page of the device. A device that has no other entry is declared by a
   [`sensor:` entry](configuration/sensor.md).

A reading that comes from a heater's register has no recipe and no offset: its row names the register
(`HeatingFunction`), and the register table in `proto_heating.cpp` names its codec. The rest is the
same.

Tests sit on the lowest stage that fits. The codec and the lookup are tested without a radio. The
schedule and the value store are tested on the hub with a reading table and recipe table of the test's
own (`tests/hub/hub_device_reading_test.cpp` shows how), since the production tables have no row until
a device needs one.

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
