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
[ADR 0102](adr/0102-a-device-without-a-main-entity-is-declared-by-a-sensor-entry-and-its-data-object-readings-run-on-a-schedule.md),
[ADR 0104](adr/0104-a-heaters-poll-reads-what-its-reading-rows-select.md),
[ADR 0105](adr/0105-every-device-value-write-is-judged-by-one-rule-per-register.md),
[ADR 0106](adr/0106-a-water-heaters-values-are-readings-of-the-same-table.md) and
[ADR 0107](adr/0107-a-covers-periodic-readings-and-battery-state-are-reading-rows-on-one-schedule.md).

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
   is kept in the device's reading store by reading, whichever the source is, so nothing else holds it. Code that writes a value uses `store_reading()`, `clear_reading()` and `mark_reading_absent()`; it never reaches into `IoDevice::reading_values`.
   For a data object the source is a `DataObjectSource`: the recipe, the offset, the byte range of the
   value in the data read and the codec. The other kinds are an `EventSource` (the answer to an event
   subscription: the event, its data length, the offset and form of the value and a factor to natural
   units), a `LimitationSource` (the answer to a limitation read) and a `StatusBatterySource` (the
   battery byte of a status reply). A reading read on a schedule (a data object, an event or a limitation)
   also says what its value becomes after repeated misses (`ReadingStale`): kept, as a counter is, or
   dropped, as a state is. Value shapes that exist are in `proto_value_codecs.h`;
   a counter that starts with a marker byte (`80` then a big-endian number, `81 FF FF` for "no value")
   is `MARKED_U16_BE`, and signed tenths of a degree behind the same marker are `MARKED_TENTHS_S16_BE`.
   A new shape is one codec enumerator, one case in `decode_value()` and its test
   in `tests/proto/value_codecs_test.cpp`. A reading whose values have names (a level, a mode) gives the
   row a `value_name` function, which the entity that shows it publishes instead of the number;
   `reading_value_kind()` then says the reading is named, a flag when its codec is a flag, and a number
   otherwise.

   **Poll policy.** A register row also says when the device's poll reads it, and the poll
   reads nothing else:

   | `ReadingPoll` | The poll reads it | Use it for |
   |---|---|---|
   | `EVERY_POLL` | Every poll; the first read that fails ends the poll | What the card shows |
   | `EVERY_POLL_MAY_MISS` | Every poll, after the readings above; a miss keeps the value and costs a read, not the poll | A setting the heater's own controller changes, a part a model may lack |
   | `ONCE_AFTER_BOOT` | Until it holds a value | A property that rarely changes |
   | `WHEN_ASKED` | Where an entity of the YAML asks for it, until the heater answers it empty | An opt-in reading |
   | `NOT_POLLED` | Never by the poll walk | A value a status reply, a programme rule or a write's read-back delivers |

   A row applies to a device by its capability, or, for a row without one, where the device's heating
   profile reads the register (`heating_profile_reads()`, from the profile's own fields); a device
   without a heating profile, such as a water heater, is selected by capability alone
   (`device_poll_reads()`). Within one policy the order of the rows is the order of the reads, so put
   a new row where its register belongs; the order of the phases is the controller's. A register that
   several rows are parts of is read once. A row read only while another register holds a value
   (`only_while`) is skipped, and its earlier value dropped, while it does not. A data-object, event or
   limitation reading ignores the policy and runs on its own schedule.
4. **Add the mirror row.** The same reading by name in `DEVICE_READINGS` in `hub_validators.py`: its
   capability (`None` for a reading no key declares), its source (`register`, `derived`, `data_object`,
   `status`, `status_parameter`, `event`, `limitation` or `status_battery`), the platform that offers its
   key or companion (`climate`, `water_heater`, `sensor` or `cover`; the sync test derives it from the
   row's capability and source, and the heater's `energy:` and the water heater's are two rows on two
   platforms), the entity kind (`sensor`, `binary_sensor`, `switch` or `text_sensor`; it has to fit the
   row's value kind), the Home Assistant unit, device class and state class, and for a data object the
   default `update_interval_ms`, at least five minutes. An entity is declared one of two ways: by a YAML
   `key`, or as a companion that every device with the capability gets, with `companion` (the name suffix
   after the main entity's name), `companion_id` (the ID suffix) and, if it should start hidden,
   `disabled_by_default`; a row has a key or a companion, never both. A cover's reading has no capability: its
   `opt_in_key` is the key of the cover whose presence declares it (an event or limitation reading reads on the
   interval that key holds), and its companion is made when the key is set. Add the capability's hint to
   `READING_FEATURE_HINTS` so the error for a kind without it says what to declare. `tests/sync/device_features_sync_test.cpp`
   fails until both tables agree.
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

A reading that comes from a register of a heater or a water heater has no recipe and no offset: its row
names the register (`DeviceRegister`). Name a new register enumerator after the value the register holds;
the Python mirror's `registers` field uses the same name. The register table in `proto_heating.cpp` names
the register's family (the high byte of its number: `0x01` for a heater, `0x00` for a water heater), its
codec and its `action_name`. The `action_name` is the register's label in the log and, for a register the
action offers, the function name a user types in the `heating_control` action. The family also decides
which devices the register is sent to (`device_takes_register()`). A register that holds
several facts in different bytes has no codec of its own: each fact is a reading whose
`RegisterSource` names a `part` (the first byte, the length and a codec, `ANY_NONZERO` for a flag that
is on when any byte of the slice is), and one read of the register stores them all. The rest is the
same. A reading that a status reply carries is never polled: the heating level and the status setpoint
are a `StatusMainParameterSource`, and a functional parameter (FP9-FP16, the whole word or one two-bit
field of it) is a `StatusFunctionalParameterSource`.

A register that can be written has one rule, in the same row of `proto_heating.cpp`: its codec and bound,
a `write_range` where it is a temperature, a `refuses` predicate where the register refuses a value its
codec holds (the mode register refuses the external-setpoint mode), and, for a register shown on a scale of
its own, the scale. Every caller asks `heating_write_accepts()` and states a range through
`heating_write_range()`; none keeps a bound or a list of its own
([ADR 0105](adr/0105-every-device-value-write-is-judged-by-one-rule-per-register.md)). A function the
`heating_control` action offers carries a label for its value in the same row, and the action finds it
by name. A new value shape is an `encode_value()` case beside the `decode_value()` one, with a test in
`tests/proto/value_codecs_test.cpp`.

Tests sit on the lowest stage that fits. The codec and the lookup are tested without a radio. The
schedule and the value store are tested on the hub with a reading table and recipe table of the test's
own (`tests/hub/hub_device_reading_test.cpp` shows how), so they do not move when a production row changes; the
Atlantic heat pump's energy counters are the production data-object readings, and the event, limitation and status
readings of a cover have production rows.

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
