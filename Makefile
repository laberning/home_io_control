# === Makefile for Home IO Control ===============================================
# Default goal: compile firmware for the default device (v2 / sx1276)
.DEFAULT_GOAL := compile


# === Recipe fragments (shared) ==========================================================
# $1 is the full config-file stem (e.g., heltec-wifi-lora-32-v2, t3s3-lr1121).
define compile_recipe
	@file="$1.yaml"; \
	echo "Compiling $$$$file"; \
	docker compose run --rm esphome compile "$$$$file"
endef

define upload_recipe
	@file="$1.yaml"; \
	echo "Uploading to $$$$file"; \
	docker compose run --rm esphome run "$$$$file"
endef

# `esphome run` (used by upload_recipe above) already does compile + upload + logs in
# one shot, so run-<suffix> is just a more honestly-named alias for upload-<suffix>.
define run_recipe
	@file="$1.yaml"; \
	echo "Compiling, uploading, and streaming logs for $$$$file"; \
	docker compose run --rm esphome run "$$$$file"
endef

define logs_recipe
	@file="$1.yaml"; \
	echo "Streaming logs from $$$$file"; \
	docker compose run --rm esphome logs "$$$$file"
endef

define clean_recipe
	@file="$1.yaml"; \
	echo "Cleaning build artifacts for $$$$file"; \
	docker compose run --rm esphome clean "$$$$file"
endef

# === Explicit device-variant targets (for tab completion & direct invocation) ===
# Each entry pairs a full config-file stem with the short target suffix used for
# compile-<suffix> / upload-<suffix> / etc. The Heltec entries keep the exact target
# names (compile-v2, ...) that existed before the T3-S3 board was added; only the
# recipe fragments above changed shape (full stem instead of a hardcoded prefix) to
# make room for stems that don't share the "heltec-wifi-lora-32-" prefix.
DEVICE_VARIANTS := heltec-wifi-lora-32-v2:v2 \
                    heltec-wifi-lora-32-v2-monitor:v2-monitor \
                    heltec-wifi-lora-32-v3:v3 \
                    heltec-wifi-lora-32-v3-monitor:v3-monitor \
                    t3s3-lr1121:t3 \
                    t3s3-lr1121-monitor:t3-monitor

variant_stem = $(word 1,$(subst :, ,$1))
variant_suffix = $(word 2,$(subst :, ,$1))

$(foreach v,$(DEVICE_VARIANTS),$(eval compile-$(call variant_suffix,$(v)): ; $(call compile_recipe,$(call variant_stem,$(v)))))
$(foreach v,$(DEVICE_VARIANTS),$(eval upload-$(call variant_suffix,$(v)): ; $(call upload_recipe,$(call variant_stem,$(v)))))
$(foreach v,$(DEVICE_VARIANTS),$(eval logs-$(call variant_suffix,$(v)): ; $(call logs_recipe,$(call variant_stem,$(v)))))
$(foreach v,$(DEVICE_VARIANTS),$(eval clean-$(call variant_suffix,$(v)): ; $(call clean_recipe,$(call variant_stem,$(v)))))
$(foreach v,$(DEVICE_VARIANTS),$(eval run-$(call variant_suffix,$(v)): ; $(call run_recipe,$(call variant_stem,$(v)))))

# === Convenience defaults (delegate to v2) ====================================
compile: compile-v2
upload: upload-v2
logs: logs-v2
clean: clean-v2
run: run-v2

# Remove generated docs (doxygen HTML, pre-processed markdown, downloaded tools)
clean-docs:
	rm -rf docs/doxygen/ build/docs/ build/doxygen-resources/


# === QA targets ================================================================

dashboard:
	docker compose up

# Formatting
format:
	@echo "Formatting C++ source files with clang-format..."
	find components tests -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +

format-check:
	@echo "Checking C++ formatting with clang-format (--dry-run)..."
	find components tests -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format --dry-run --Werror {} +

# YAML linting (safe selection, excludes generated .esphome)
yamllint:
	@echo "Linting YAML configuration files..."
	yamllint config/tests/*.yaml config/*.yaml tests/corpus/captures/ .github/workflows/*.yml .github/dependabot.yml

# Golden-frame corpus: schema + self-consistency validation of every capture YAML
# (CRC, CTRL0 length, duplicate ids), plus the ingest/build/validate tool self-tests.
# The generated C++ headers are used for unit-tests
corpus-validate:
	@echo "Validating golden-frame corpus captures..."
	@python3 scripts/corpus/validate.py
	@python3 scripts/corpus/tests/run_tests.py
	@python3 scripts/lr1121_firmware/tests/run_tests.py

# libFuzzer target for the frame parser and its pure decoders, seeded from the golden-frame
# corpus. Time-boxed background check (FUZZ_TIME seconds, default 60) — not part of `make check`/
# `make test`, since fuzzing doesn't have a pass/fail gate the way a fixed test suite does. The
# parser TUs (proto_frame/proto_codecs/proto_device_model) are ESPHome-free, so this links
# directly with clang, no stubs needed.
FUZZ_TIME ?= 60
fuzz-frame:
	@mkdir -p build/fuzz/seeds build/fuzz/corpus
	@python3 scripts/corpus/extract_fuzz_seeds.py
	clang++ -std=c++17 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -g -O1 \
		-Icomponents/home_io_control \
		components/home_io_control/proto_frame.cpp components/home_io_control/proto_codecs.cpp \
		components/home_io_control/proto_device_model.cpp tests/fuzz/fuzz_frame_parse.cpp \
		-o build/fuzz/fuzz_frame_parse
	./build/fuzz/fuzz_frame_parse -max_total_time=$(FUZZ_TIME) -print_final_stats=1 \
		build/fuzz/corpus build/fuzz/seeds

# Static analysis (local clang-tidy after building inside Docker)
clang-tidy:
	@echo "Running clang-tidy..."
	@scripts/run-clang-tidy.sh

# Cross-language check: tuning parameter names must match between tuning.py and the C++ registry
tuning-sync:
	@echo "Checking tuning parameter sync (tuning.py <-> tuning_registry.cpp)..."
	@python3 scripts/check-tuning-sync.py

# Detects (and reports) config/tests/.esphome/build/<env>/ dirs with a stale or
# half-regenerated object cache; see scripts/check-build-cache.py. Wired automatically
# (with --clean) into firmware-test and run-clang-tidy.sh, so this is mainly for manual
# inspection.
check-build-cache:
	@python3 scripts/check-build-cache.py

# Cross-file check: every relative markdown link across tracked docs must resolve.
docs-link-check:
	@echo "Checking documentation cross-links..."
	@python3 scripts/check-docs-links.py

# Wipes config/tests/.esphome/build/<env>/ for every config/tests/test-*.yaml config (the ones
# make clang-tidy / firmware-test / check build against), plus config/tests/.esphome/storage/
# (per-config validated-YAML cache, keyed by config filename rather than device_name, so it's
# wiped in one shot instead of per-env). Uses Docker rather than a host-side rm — these paths are
# root-owned (created by a previous Docker run), so a plain `rm` on the host fails with Permission
# denied. This is the manual full-wipe hammer; scripts/check-build-cache.py now detects and
# auto-cleans the targeted per-env staleness case automatically (see check-build-cache above), so
# reach for this instead when you want a clean slate (e.g. storage/ permission issues, or a hunch
# the automated check missed something).
clean-test-cache:
	@echo "Cleaning test build caches in config/tests/.esphome/build/"
	@for cfg in config/tests/test-*.yaml; do \
	  name=$$(grep 'device_name:' "$$cfg" | head -1 | cut -d: -f2- | sed "s/['\"]//g" | xargs); \
	  if [ -z "$$name" ]; then echo "ERROR: Could not extract device_name from $$cfg"; exit 1; fi; \
	  echo "=== Cleaning config/tests/.esphome/build/$$name ==="; \
	  docker compose run --rm --entrypoint sh esphome -c "rm -rf /config/tests/.esphome/build/$$name" || exit 1; \
	done
	@echo "Cleaning config/tests/.esphome/storage/"
	@docker compose run --rm --entrypoint sh esphome -c "rm -rf /config/tests/.esphome/storage" || exit 1

# Compilation tests for all platform configs.
# test-esp32-lr1121-fwupdate.yaml is excluded here and built by firmware-test-fwupdate instead:
# it is the only config that downloads firmware images at build time, so keeping it out of this
# loop keeps `make check` runnable without network access.
firmware-test:
	@python3 scripts/check-build-cache.py --clean
	@echo "Compiling test configurations in config/tests/"
	@for cfg in config/tests/test-*.yaml; do \
	  name=$$(basename "$$cfg"); \
	  case "$$name" in test-esp32-lr1121-fwupdate.yaml) echo "=== Skipping $$name (needs network; make firmware-test-fwupdate) ==="; continue;; esac; \
	  echo "=== Compiling $$name ==="; \
	  docker compose run --rm esphome compile "/config/tests/$$name" || exit 1; \
	done

# Compile test for the LR1121 firmware-update + bootloader-rewrite features. Separate from
# firmware-test because it fetches two images from GitHub. Without it, IOHOME_LR1121_FIRMWARE_UPDATE
# and IOHOME_LR1121_BOOTLOADER_UPDATE are only ever compiled by the x86-64 host unit-test build --
# a different compiler with different type widths from the Xtensa target (this repo has already
# been bitten once by uint32_t being `long` there).
firmware-test-fwupdate:
	@echo "=== Compiling test-esp32-lr1121-fwupdate.yaml (downloads firmware images) ==="
	@docker compose run --rm esphome compile "/config/tests/test-esp32-lr1121-fwupdate.yaml"

# === Unit test configuration ===================================================

# All component source files needed for tests (protocol + platform + hub + radio drivers)
COMPONENT_SRCS := $(wildcard components/home_io_control/*.cpp)

# Test stubs (only global symbols — App, global_preferences, fnv1_hash)
STUB_SRCS := tests/stubs/stubs.cpp

# All test files (*_test.cpp) in tests/ root
TEST_SRCS := $(wildcard tests/*_test.cpp)

# Include paths (build/corpus holds the generated golden-frame corpus header — see corpus-gen below)
INCLUDES := -Icomponents/home_io_control \
            -Itests/include \
            -Itests/support \
            -Ibuild/corpus

# Mirror the ESPHome API defines that the component injects during firmware codegen.
# Without these, host builds silently compile out the rename-action registration path and
# the associated unit tests only exercise a reduced slice of the recovered feature.
# IOHOME_LR1121_FIRMWARE_UPDATE is normally only set when a YAML config has a
# lr1121_firmware_update: block, but host tests need it unconditionally: without it,
# COMPONENT_SRCS's wildcard picks up radio_lr1121_firmware_updater.cpp and hub_lr1121_firmware_update.cpp
# and compiles each to an empty TU, so their tests would have nothing to link against.
# IOHOME_LR1121_BOOTLOADER_UPDATE is the same story one level down: normally only set when a
# `bootloader:` sub-block is configured, but the 0x81xx primitives and the three-stage
# orchestration it gates need to be always-on for their host tests to have anything to link.
UNIT_TEST_DEFINES := -DUSE_API_USER_DEFINED_ACTIONS \
			 -DUSE_API_CUSTOM_SERVICES \
			 -DUSE_API_HOMEASSISTANT_SERVICES \
			 -DIOHOME_LR1121_FIRMWARE_UPDATE \
			 -DIOHOME_LR1121_BOOTLOADER_UPDATE

# Regenerates build/corpus/corpus_generated.h from tests/corpus/captures/**/*.yaml.
# see tests/corpus/README.md.
corpus-gen:
	@mkdir -p build
	@python3 scripts/corpus/build.py

# --- Host unit-test build (incremental; objects mirror source paths) ---
# HOST_VARIANT selects the object/binary tree; "asan" adds sanitizer flags (see unit-test-asan).
HOST_VARIANT ?= default
HOST_BUILD_DIR := build/host/$(HOST_VARIANT)
HOST_OBJ_DIR := $(HOST_BUILD_DIR)/obj
HOST_EXTRA_FLAGS ?=
HOST_CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable \
                 -Wno-unused-variable -Wno-reorder -DIRAM_ATTR= \
                 $(UNIT_TEST_DEFINES) $(INCLUDES) $(HOST_EXTRA_FLAGS)

HOST_SRCS := $(COMPONENT_SRCS) $(STUB_SRCS) $(TEST_SRCS)
HOST_OBJS := $(patsubst %.cpp,$(HOST_OBJ_DIR)/%.o,$(HOST_SRCS))
HOST_DEPS := $(HOST_OBJS:.o=.d)
HOST_TEST_BIN := $(HOST_BUILD_DIR)/test_home_io_control

$(HOST_OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	g++ $(HOST_CXXFLAGS) -MMD -MP -c $< -o $@

$(HOST_TEST_BIN): $(HOST_OBJS)
	g++ $(HOST_CXXFLAGS) $^ -lgtest -lgtest_main -pthread -o $@

-include $(HOST_DEPS)

# Builds $(HOST_TEST_BIN) for the current HOST_VARIANT and runs it. Shared by unit-test and
# unit-test-asan (invoked as a sub-make with HOST_VARIANT/HOST_EXTRA_FLAGS overridden on the
# command line, which is what makes $(HOST_TEST_BIN) resolve to the right variant tree below).
host-run: $(HOST_TEST_BIN)
	@echo "Running tests ($(HOST_VARIANT))..."
	@UBSAN_OPTIONS=print_stacktrace=1 \
		LSAN_OPTIONS=suppressions=$(CURDIR)/tests/asan/lsan.supp \
		./$(HOST_TEST_BIN)

# Public entry point: corpus-gen runs first (ordering!), then a parallel sub-make builds/links.
# Parallelism is scoped to this sub-make (not a global -j) so the docker/esphome targets in
# this Makefile stay serial. After changing HOST_CXXFLAGS, run `make clean-host` — make does
# not hash command lines, so flag-only changes don't invalidate existing objects.
unit-test: corpus-gen
	@echo "Building Google Test unit tests for home_io_control (host-only)..."
	@$(MAKE) --no-print-directory -j$(shell nproc) host-run

# ASan/UBSan variant: same sources and rules as unit-test, built into a fully separate object
# tree (build/host/asan/) so the two variants never poison each other. Uninstrumented system
# libgtest is fine to link against — ASan intercepts allocation globally.
ASAN_HOST_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -fno-omit-frame-pointer

unit-test-asan: corpus-gen
	@echo "Building Google Test unit tests for home_io_control (ASan/UBSan)..."
	@$(MAKE) --no-print-directory -j$(shell nproc) \
		HOST_VARIANT=asan HOST_EXTRA_FLAGS="$(ASAN_HOST_FLAGS)" host-run

clean-host:
	rm -rf build/host


# === Documentation =============================================================

DOXYGEN_OUTPUT := docs/doxygen

doxygen:
	@echo "Generating Doxygen documentation..."
	@scripts/generate-doxygen.sh


# === Composite targets =========================================================

# Every sub-target of `lint` below must have a matching CI job in .github/workflows/ci.yml,
# so a local-only check can never silently drift out of CI coverage:
#   format-check     -> format
#   yamllint         -> yamllint
#   clang-tidy       -> tidy
#   tuning-sync      -> tuning-sync
#   corpus-validate  -> corpus-validate
#   docs-link-check  -> docs-link-check
lint: format-check yamllint clang-tidy tuning-sync corpus-validate docs-link-check
test: unit-test unit-test-asan firmware-test
check: lint test doxygen

# Backward compatibility aliases (deprecated, use new names)
test-compile: firmware-test
test-unit: unit-test


# === Phony declarations ========================================================

.PHONY: dashboard \
		format format-check yamllint clang-tidy tidy tuning-sync corpus-validate corpus-gen \
		docs-link-check \
		fuzz-frame \
		firmware-test unit-test unit-test-asan host-run clean-host lint test check \
		test-compile test-unit \
		doxygen clean-docs clean-test-cache
