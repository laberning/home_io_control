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

```bash
# Run host-based Google Test unit tests (no ESP32 needed)
make unit-test

# Compile all platform configurations (firmware test)
make firmware-test

# Run all tests (unit + firmware compilation)
make test

# Full QA: lint + tests
make check

# Validate the golden-frame corpus (schema, CRC, crypto — see tests/corpus/README.md)
make corpus-validate

# Regenerate the corpus's generated C++ fixture header (also runs automatically before unit-test)
make corpus-gen

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

# Compile the LR1121 config (LilyGO T3-S3 LR1121 variant)
make compile-t3

# Compile and flash via USB
make upload          # SX1276
make upload-v3       # SX1262
make upload-t3       # LR1121

# Compile, flash, and stream logs in one shot (alias for upload-*, since
# `esphome run` already does all three)
make run              # SX1276
make run-v3           # SX1262
make run-t3           # LR1121

# Monitor serial output
make logs            # SX1276
make logs-v3         # SX1262
make logs-t3         # LR1121

# Clean build artifacts
make clean           # SX1276
make clean-v3        # SX1262
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

## See also

- [Contributing](contributing.md) — hardware testing, device reports, and pull requests
- [Architecture overview](architecture_overview.md) — how the component is put together
