# Development setup
<!-- doxygen-label: development_setup -->

Everything you need to build, test and flash this component locally. You only need this page if
you are changing the code — testing a device against a released build needs none of it.

## Setup and prerequisites

The build system uses Docker for firmware compilation and host tools for testing and linting.
After setup, run `make check` to verify the full toolchain.

### Ubuntu / Debian

```bash
sudo apt-get update && sudo apt-get install -y clang-format clang-tidy yamllint libgtest-dev
# The Bridle tools (session linter, bench scripts) come from a pinned wheel that `make tools` installs into
# build/tools/venv; while the Bridle release is private pass BRIDLE_WHEEL=<wheel>, or build it in a sibling
# ../bridle checkout (`make release`), whose dist/ is used when it holds the pinned wheel
sudo apt-get install -y python3-venv
# Optional: for API documentation generation
sudo apt-get install graphviz python3-pygments
```

### macOS (Homebrew)

```bash
brew install clang-format llvm yamllint googletest

# Optional: for API documentation generation
brew install graphviz pygments
```

### Windows (WSL2)

Use the Ubuntu command above inside a WSL2 distribution.

## Testing

Host tests live in one directory per layer: `tests/proto/`, `tests/radio/`, `tests/hub/`,
`tests/platform/`, `tests/oneway/` and `tests/tuning/` test that layer's code; `tests/corpus/` replays
the golden-frame corpus, `tests/sync/` checks that C++ and Python tables agree, and `tests/harness/`
tests the host stubs themselves. The doubles the suites share are in `tests/support/` (a hub-free `HeaterController` fixture, the hub port doubles, timed replay, a log capture that counts the lines a call logs) and `tests/stubs/` (scripted SPI, the software-PHY test driver); `tests/include/` holds the ESPHome header stubs. Put a new `*_test.cpp` in the directory of the code it tests; the
Makefile finds it there. Any compiler warning fails the host build.

`tests/fullstack/` is the exception: its tests need a Bridle simulator, a program that speaks the
[simulator control channel](https://github.com/laberning/bridle/blob/bridle-v0.7.0/README.md#111-simulator-control-channel)
of the Bridle specification and has an 802.15.4 radio. `make fullstack` builds and runs them against the
simulator that `BRIDLE_SIM` names, and `make unit-test` does not build them.

```bash
# Run host-based Google Test unit tests (no ESP32 needed)
make unit-test

# Compile all platform configurations (firmware test)
make firmware-test

# Codegen tests: validators and accept/reject YAML fixtures, run in the ESPHome container.
# Every function that raises cv.Invalid needs a covering case, or this fails and names it.
make py-test

# Run all tests (unit, ASan, codegen, firmware compilation)
make test

# Full QA: lint + tests
make check

# Check the host-test ESPHome stubs against the real ESPHome headers (in the container; part of lint)
make stub-sync

# Check that the committed Bridle headers and the wheel pin match BRIDLE_VERSION (part of lint)
make bridle-check

# Take a newer Bridle release (see scripts/bridle-update.sh)
scripts/bridle-update.sh <release tag>

# Lint the recorded Bridle sessions (part of lint), and the traces the unit tests write (part of make test)
make bridle-session-check
make bridle-trace-check

# Regenerate the recorded sessions from the host tests that record them; review the diff before committing
make bridle-sessions-regen

# Validate the golden-frame corpus (schema, CRC, crypto — see tests/corpus/README.md) and run the
# unit tests of the corpus and bench scripts (scripts/bench/)
make corpus-validate

# Regenerate the corpus's generated C++ fixture header (also runs automatically before unit-test)
make corpus-gen

# Fuzz the frame parser and the software-PHY RX decoder, seeded from the corpus
# (time-boxed, FUZZ_TIME seconds, default 60; not part of `make check`, runs weekly in CI)
make fuzz-frame
make fuzz-soft-phy
make fuzz-bridle-codec
make fuzz-io2g4-unwrap

# Full-stack tests: the hub's exchange engine over the Bridle driver and a simulated co-processor,
# with an emulated blind answering on the simulated air. Not part of `make check`; CI only compiles
# them (make fullstack-build)
make fullstack BRIDLE_SIM=<path to the simulator binary>

# Clean stale build caches for config/tests/*.yaml (fixes confusing linker errors after
# adding a new .cpp under components/home_io_control/)
make clean-test-cache
```

## Firmware build and flash

```bash
# Compile the firmware (SX1276 / Heltec V2)
make compile

# Compile the SX1262 validation config (Heltec V3)
make compile-v3

# Compile the Heltec V4 configs (SX1262 + front-end module); `v4` is the V4.3
make compile-v4-2
make compile-v4-3
make compile-v4

# Compile the LR1121 config (LilyGO T3-S3 LR1121 variant)
make compile-t3

# Compile the configs of an ESP32-C3 SuperMini with a radio co-processor: 2.4 GHz and 868 MHz
make compile-c3
make compile-c3-868

# Compile and flash via USB
make upload          # SX1276
make upload-v3       # SX1262
make upload-v4       # SX1262 + FEM (V4.3); also upload-v4-2, upload-v4-3
make upload-t3       # LR1121

# Compile, flash, and stream logs in one shot (alias for upload-*, since
# `esphome run` already does all three)
make run              # SX1276
make run-v3           # SX1262
make run-v4           # SX1262 + FEM (V4.3); also run-v4-2, run-v4-3
make run-t3           # LR1121

# Monitor serial output
make logs            # SX1276
make logs-v3         # SX1262
make logs-v4         # SX1262 + FEM (V4.3); also logs-v4-2, logs-v4-3
make logs-t3         # LR1121

# Clean build artifacts
make clean           # SX1276
make clean-v3        # SX1262
make clean-v4        # SX1262 + FEM (V4.3); also clean-v4-2, clean-v4-3
make clean-t3        # LR1121

# Format all C++ source files
make format

# Build the documentation site (requires graphviz; doxygen is auto-downloaded).
# docs/*.md and docs/adr/*.md are plain GitHub-flavoured Markdown; the build
# stages them into doxygen syntax (page labels, nested trees) — nothing
# doxygen-specific is committed. See scripts/stage-docs.py. Published to
# https://laberning.github.io/home_io_control/ on every push to main.
make doxygen

# Start ESPHome dashboard on port 6052
make dashboard
```

## Bench configurations and scripts

The bench tools measure the bridge against real radios. None of them is part of a user's setup, and
none addresses a real device.

| Config (`make compile-…`) | What it is |
|---|---|
| `bench868-ref-v3.yaml` (`ref868-v3`) | A Heltec V3 (SX1262) as the 868 MHz reference radio: API actions to send a frame, an unmodulated carrier or a bare preamble, and to listen. It replaces that board's hub, so flash it only on a board set aside for the bench. |
| `esp32c3-supermini-bridle-868-phantom.yaml` (`c3-868-phantom`) | The 868 MHz bridge with two covers bound to made-up node ids, so no real receiver can be addressed. |
| `esp32c3-supermini-bridle-bench.yaml` (`c3-bench`) | A hub with one cover that a stand-in device answers, for the answer-delay measurement; commanded over HTTP. |
| `esp32c3-supermini-bridle-coex.yaml` (`c3-coex`) | The same at the co-processor's maximum transmit power, with the Wi-Fi signal reported, for the Wi-Fi coexistence measurement. |
| `esp32c3-supermini-idle.yaml` (`c3-idle`) | Wi-Fi, API and OTA only, never speaking to the co-processor, so a PC can use the co-processor over USB. Flash the bridge config again to go back. |

The scripts in `scripts/bench/` run in the tools venv (`build/tools/venv/bin/python`); `make corpus-validate`
runs their unit tests against an in-process fake co-processor.

- `io2g4.py` drives a co-processor as a 2.4 GHz controller from a PC (`poll`, `execute`), or as a stand-in
  device (`respond`) that answers a hub and prints what it heard.
- `respond_report.py` reads that `respond` output and summarises each EXECUTE: the wake-up train, the
  CTRL1 of the request and of the challenge answer, and the time from the end of the challenge to the answer.
- `ref868.py` drives the 868 MHz reference radio over the ESPHome API. It runs in the ESPHome container,
  which has the API client; mount the script directory, since the container sees only `config/` and `components/`:

```bash
docker compose run --rm -v "$PWD/scripts/bench:/bench:ro" --entrypoint python3 esphome /bench/ref868.py --help
```

## See also

- [Contributing](contributing.md) — hardware testing, device reports, and pull requests
- [Architecture overview](architecture_overview.md) — how the component is put together
