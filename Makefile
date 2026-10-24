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
#
# `v4` is an alias for `v4-3` (Heltec V4.3); `v4-2` and `v4-3` name each revision unambiguously.
DEVICE_VARIANTS := heltec-wifi-lora-32-v2:v2 \
                    heltec-wifi-lora-32-v2-monitor:v2-monitor \
                    heltec-wifi-lora-32-v3:v3 \
                    heltec-wifi-lora-32-v3-monitor:v3-monitor \
                    heltec-wifi-lora-32-v4-2:v4-2 \
                    heltec-wifi-lora-32-v4-3:v4-3 \
                    heltec-wifi-lora-32-v4-3:v4 \
                    t3s3-lr1121:t3 \
                    t3s3-lr1121-monitor:t3-monitor \
                    esp32c3-supermini-bridle:c3 \
                    esp32c3-supermini-bridle-bench:c3-bench \
                    esp32c3-supermini-bridle-868:c3-868

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

# Formatting. CLANG_FORMAT picks the binary; CI pins clang-format-21 (the version an Ubuntu 26.04
# dev machine installs as plain `clang-format`), because versions disagree on comment alignment.
CLANG_FORMAT ?= clang-format

# The generated Bridle headers (scripts/bridle-update.sh) keep the formatting of their release: both
# targets leave them out.
format:
	@echo "Formatting C++ source files with clang-format..."
	find components tests -type f \( -name '*.cpp' -o -name '*.h' \) -not -name radio_bridle_enums.h -not -path 'tests/bridle/*' \
		-exec $(CLANG_FORMAT) -i {} +

format-check:
	@echo "Checking C++ formatting with clang-format (--dry-run)..."
	find components tests -type f \( -name '*.cpp' -o -name '*.h' \) -not -name radio_bridle_enums.h -not -path 'tests/bridle/*' \
		-exec $(CLANG_FORMAT) --dry-run --Werror {} +

# YAML linting (safe selection, excludes generated .esphome)
yamllint:
	@echo "Linting YAML configuration files..."
	yamllint config/tests/*.yaml config/boards/*.yaml config/*.yaml tests/corpus/captures/ tests/python/fixtures/ \
		.github/workflows/*.yml .github/dependabot.yml

# Golden-frame corpus: schema + self-consistency validation of every capture YAML
# (CRC, CTRL0 length, duplicate ids), plus the ingest/build/validate tool self-tests.
# The generated C++ headers are used for unit-tests
corpus-validate: tools
	@echo "Validating golden-frame corpus captures..."
	@$(PY) scripts/corpus/validate.py
	@$(PY) scripts/corpus/tests/run_tests.py
	@python3 scripts/lr1121_firmware/tests/run_tests.py
	@echo "Linting the Bridle session fixtures..."
	@$(BRIDLE_TOOLS_BIN)/bridle-session validate tests/corpus/sessions/*.yaml
	@for f in tests/corpus/sessions/*.yaml; do $(BRIDLE_TOOLS_BIN)/bridle-session check "$$f" || exit 1; done

# libFuzzer targets, seeded from the golden-frame corpus. Time-boxed background checks
# (FUZZ_TIME seconds, default 60), not part of `make check`/`make test`: fuzzing has no pass/fail
# gate the way a fixed test suite does. Each target keeps its own seed and working-corpus
# directories under build/fuzz/, so inputs of different shapes never mix.
#   fuzz-frame     frame parser + its pure decoders (proto_frame/codecs/device_model/crypto, all
#                  ESPHome-free; tests/include only supplies esp_random.h for proto_crypto.cpp).
#   fuzz-bridle-codec  Bridle framing, message decode, TLV and `ready` parsing (radio_bridle_codec.cpp);
#                  seeded from the Bridle release's vectors.
#   fuzz-io2g4-unwrap  2.4 GHz PSDU unwrap (radio_io2g4_phy.cpp), seeded from the io_2g4 captures.
#   fuzz-soft-phy  software-PHY RX decoder (radio_soft_phy.cpp): raw chip buffers through the
#                  length peek, UART probe and CRC recovery. radio_soft_phy.h reaches
#                  esphome/core/hal.h through radio_interface.h, which tests/include stubs.
# A new target is one FUZZ_RUN call: name, extract_fuzz_seeds.py flags, sources.
FUZZ_TIME ?= 60
FUZZ_CXX ?= clang++
FUZZ_CXXFLAGS := -std=c++20 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -g -O1 \
	-Icomponents/home_io_control -Itests/include -Itests/bridle -DIOHOME_BRIDLE

# $(1) = binary/target name, $(2) = extract_fuzz_seeds.py flags, $(3) = seed dir name, $(4) = sources
define FUZZ_RUN
	@mkdir -p build/fuzz/$(3) build/fuzz/corpus_$(1)
	@$(PY) scripts/corpus/extract_fuzz_seeds.py $(2)
	$(FUZZ_CXX) $(FUZZ_CXXFLAGS) $(4) -o build/fuzz/$(1)
	./build/fuzz/$(1) -max_total_time=$(FUZZ_TIME) -print_final_stats=1 build/fuzz/corpus_$(1) build/fuzz/$(3)
endef

fuzz-frame: tools
	$(call FUZZ_RUN,fuzz_frame_parse,,seeds,\
		components/home_io_control/proto_frame.cpp components/home_io_control/proto_codecs.cpp \
		components/home_io_control/proto_device_model.cpp components/home_io_control/proto_device_types.cpp \
		components/home_io_control/proto_crypto.cpp \
		tests/fuzz/fuzz_frame_parse.cpp)

fuzz-bridle-codec: tools
	$(call FUZZ_RUN,fuzz_bridle_codec,--bridle,seeds_bridle,\
		components/home_io_control/radio_bridle_codec.cpp components/home_io_control/proto_frame.cpp \
		tests/fuzz/fuzz_bridle_codec.cpp)

fuzz-io2g4-unwrap: tools
	$(call FUZZ_RUN,fuzz_io2g4_unwrap,--io2g4,seeds_io2g4,\
		components/home_io_control/radio_io2g4_phy.cpp components/home_io_control/radio_bridle_codec.cpp \
		components/home_io_control/proto_frame.cpp tests/fuzz/fuzz_io2g4_unwrap.cpp)

fuzz-soft-phy: tools
	$(call FUZZ_RUN,fuzz_soft_phy_rx,--soft-phy,seeds_soft_phy,\
		components/home_io_control/radio_soft_phy.cpp components/home_io_control/proto_frame.cpp \
		tests/fuzz/fuzz_soft_phy_rx.cpp)

# Static analysis (local clang-tidy after building inside Docker)
clang-tidy:
	@echo "Running clang-tidy..."
	@scripts/run-clang-tidy.sh
	@echo "Running clang-tidy on the Bridle build (the sources compiled only under IOHOME_BRIDLE)..."
	@scripts/run-clang-tidy.sh config/tests/test-esp32-bridle.yaml
	@scripts/run-clang-tidy.sh config/tests/test-esp32-bridle-868.yaml

# Cross-language check: tuning parameter names must match between tuning.py and the C++ registry
tuning-sync:
	@echo "Checking tuning parameter sync (tuning.py <-> tuning_registry.cpp)..."
	@python3 scripts/check-tuning-sync.py

# Backstop scan for accidentally-committed, byte-exact key-transfer/challenge-response frames in
# tracked prose/doc-shaped and source files (*.md/*.txt/*.yaml/*.yml/*.cpp/*.h/*.hpp outside
# tests/corpus/captures/, which has its own stronger crypto-verifying gate above), plus a check
# that no *_key value from a local config/secrets.yaml (if one exists) leaks verbatim into any
# tracked file. See scripts/check-key-material.py's module docstring for both algorithms.
key-material-scan:
	@echo "Scanning for accidentally-committed key material..."
	@python3 scripts/check-key-material.py
	@python3 scripts/check_key_material_test.py

# Cross-language check: this component's hand-built paste-ready YAML emitters
# (build_oneway_adoption_report(), build_key_extraction_report(), build_device_yaml_snippet()) must
# emit keys that exist in their real ESPHome schemas -- see scripts/check-yaml-emitters.py.
yaml-emitter-sync:
	@python3 scripts/check-yaml-emitters.py

# Host-stub drift check: the ESPHome API the component uses, as declared by the stubs in
# tests/include/esphome, must match the real headers of the pinned ESPHome image (ADR 0014) --
# see scripts/check-stub-sync.py. Runs in the container, where `import esphome` finds them.
stub-sync:
	@echo "Checking host stubs against the ESPHome headers..."
	@docker compose run --rm -v "$(CURDIR):/repo:ro" -w /repo -e PYTHONDONTWRITEBYTECODE=1 \
		--entrypoint python3 esphome scripts/check-stub-sync.py

# Layering check: include direction between the component's layers (collaborators and entities
# never include hub_internal.h; only listed files reach hub_core.h; protocol and radio files include
# only their own layer) -- see scripts/check-include-graph.py.
include-graph:
	@echo "Checking include-graph layering..."
	@python3 scripts/check-include-graph.py
	@python3 scripts/check_include_graph_test.py

# Cross-source check: board pinouts documented in the published Markdown (README.md and docs/**)
# must match the single source of truth in config/boards/*.yaml -- see scripts/check-board-pinouts.py.
board-pinout-sync:
	@echo "Checking board pinout sync (docs <-> config/boards/)..."
	@python3 scripts/check-board-pinouts.py

# Detects (and reports) config/tests/.esphome/build/<env>/ dirs with a stale or
# half-regenerated object cache; see scripts/check-build-cache.py. Wired automatically
# (with --clean) into firmware-test and run-clang-tidy.sh, so this is mainly for manual
# inspection.
check-build-cache:
	@python3 scripts/check-build-cache.py

# Cross-file check: every relative markdown link across tracked docs must resolve.
docs-link-check:
	@echo "Checking documentation cross-links and staging invariants..."
	@python3 scripts/check-docs-links.py
	@python3 scripts/check_docs_links_test.py

# Prose gate for the two writing rules a link checker cannot see: naming git-excluded
# working notes as the source of a fact, and narrating this repo's own history.
# See scripts/check-docs-prose.py.
docs-prose-check:
	@echo "Checking documentation prose (provenance, history)..."
	@python3 scripts/check-docs-prose.py

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
#
# CI splits the compile across two parallel jobs (the `test-compile` matrix in ci.yml), one per
# group below. The groups are balanced by measured compile time (~5 min each on a cold runner);
# the two FEM configs share a group so the second compiles warm. Every other
# config/tests/test-*.yaml must be in exactly one group — firmware-test-groups-check fails
# otherwise, so a new config can never silently drop out of CI.
FIRMWARE_TEST_GROUP_1 := test-esp32-arduino.yaml test-esp32-esp-idf-tuning.yaml
FIRMWARE_TEST_GROUP_2 := test-esp32-bridle.yaml test-esp32-bridle-868.yaml test-esp32-esp-idf.yaml test-esp32-sx1262-fem-xy16p35.yaml test-esp32-sx1262-fem.yaml
FIRMWARE_TEST_EXCLUDED := test-esp32-lr1121-fwupdate.yaml

# Configs compiled by firmware-test; override to compile a subset, e.g.
# `make firmware-test FIRMWARE_TEST_CONFIGS="$(FIRMWARE_TEST_GROUP_1)"`.
FIRMWARE_TEST_CONFIGS ?= $(FIRMWARE_TEST_GROUP_1) $(FIRMWARE_TEST_GROUP_2)

firmware-test: firmware-test-groups-check
	@python3 scripts/check-build-cache.py --clean
	@echo "Compiling test configurations in config/tests/"
	@for name in $(FIRMWARE_TEST_CONFIGS); do \
	  echo "=== Compiling $$name ==="; \
	  docker compose run --rm esphome compile "/config/tests/$$name" || exit 1; \
	done

firmware-test-group-1:
	@$(MAKE) --no-print-directory firmware-test FIRMWARE_TEST_CONFIGS="$(FIRMWARE_TEST_GROUP_1)"

firmware-test-group-2:
	@$(MAKE) --no-print-directory firmware-test FIRMWARE_TEST_CONFIGS="$(FIRMWARE_TEST_GROUP_2)"

# Every config/tests/test-*.yaml must appear exactly once across the groups + exclusion list.
firmware-test-groups-check:
	@status=0; \
	listed=" $(FIRMWARE_TEST_GROUP_1) $(FIRMWARE_TEST_GROUP_2) $(FIRMWARE_TEST_EXCLUDED) "; \
	for cfg in config/tests/test-*.yaml; do \
	  name=$$(basename "$$cfg"); \
	  count=$$(printf '%s\n' $$listed | grep -cx "$$name"); \
	  if [ "$$count" -ne 1 ]; then \
	    echo "ERROR: $$name appears $$count times in FIRMWARE_TEST_GROUP_1/_2/EXCLUDED (Makefile); expected exactly 1"; \
	    status=1; \
	  fi; \
	done; \
	for name in $$listed; do \
	  if [ ! -f "config/tests/$$name" ]; then \
	    echo "ERROR: $$name is listed in a FIRMWARE_TEST_* group (Makefile) but config/tests/$$name does not exist"; \
	    status=1; \
	  fi; \
	done; \
	exit $$status

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

# Test stubs (only global symbols — App, global_preferences, fnv1_hash) plus per-test isolation:
# test_isolation.cpp resets test_scheduler's pending-timer registry and enforces the ManualClock
# RAII rule between tests.
STUB_SRCS := tests/stubs/stubs.cpp tests/support/test_isolation.cpp tests/support/fake_coprocessor.cpp

# All test files (*_test.cpp), one directory per layer under tests/ (proto/, radio/, hub/, platform/,
# oneway/, tuning/, corpus/, sync/, harness/). tests/fuzz/ holds libFuzzer targets, built separately.
TEST_SRCS := $(sort $(shell find tests -name '*_test.cpp' -not -path 'tests/fuzz/*' -not -path 'tests/fullstack/*'))

# The full-stack suite (tests/fullstack/) needs the simulator and is built and run by `make fullstack`
# on its own, never into the unit-test binary.
FULLSTACK_SRCS := $(sort $(wildcard tests/fullstack/*.cpp)) tests/support/io_blind_emulator.cpp tests/support/sim_bridge.cpp

# Include paths (build/corpus holds the generated golden-frame corpus header — see corpus-gen below).
# -Itests comes last: test files name shared harness headers relative to tests/ ("stubs/…",
# "support/…") from whichever layer directory they live in; component headers still win.
INCLUDES := -Icomponents/home_io_control \
            -Itests/include \
            -Itests/support \
            -Ibuild/corpus \
            -Ibuild/bridle \
            -Itests/bridle \
            -Itests

# Mirror the ESPHome API defines that the component injects during firmware codegen.
# Without these, host builds silently compile out the rename-action registration path and
# the associated unit tests only exercise a reduced slice of the recovered feature.
# IOHOME_LR1121_FIRMWARE_UPDATE is normally only set when a YAML config has a
# lr1121_firmware_update: block, but host tests need it unconditionally: without it,
# COMPONENT_SRCS's wildcard picks up radio_lr1121_firmware_updater.cpp and lr1121_firmware_update_controller.cpp
# and compiles each to an empty TU, so their tests would have nothing to link against.
# IOHOME_LR1121_BOOTLOADER_UPDATE is the same story one level down: normally only set when a
# `bootloader:` sub-block is configured, but the 0x81xx primitives and the three-stage
# orchestration it gates need to be always-on for their host tests to have anything to link.
UNIT_TEST_DEFINES := -DUSE_API_USER_DEFINED_ACTIONS \
			 -DUSE_API_CUSTOM_SERVICES \
			 -DUSE_API_HOMEASSISTANT_SERVICES \
			 -DIOHOME_LR1121_FIRMWARE_UPDATE \
			 -DIOHOME_LR1121_BOOTLOADER_UPDATE \
			 -DIOHOME_BRIDLE

# Regenerates build/corpus/corpus_generated.h from tests/corpus/captures/**/*.yaml.
# see tests/corpus/README.md.
corpus-gen:
	@mkdir -p build
	@python3 scripts/corpus/build.py

# Bridle: the generated headers of one Bridle release are committed (components/home_io_control/radio_bridle_enums.h,
# tests/bridle/) and named by BRIDLE_VERSION; scripts/bridle-update.sh is the only thing that writes them. The Python
# tools (session linter, header generator for the recorded sessions, bench scripts, fuzz seeds) come from the
# release's wheel, pinned in scripts/requirements-dev.txt, into a venv under build/tools/. While the release is not
# on GitHub, or the repository is private, BRIDLE_WHEEL=<wheel from the Bridle checkout's `make release`> installs the
# same file (its hash must equal BRIDLE_VERSION's); without it, the sibling checkout's ../bridle/dist/ is used when its
# wheel is that file. IOHOME_BRIDLE (in UNIT_TEST_DEFINES) compiles the Bridle sources.
TOOLS_VENV := build/tools/venv
BRIDLE_TOOLS_BIN := $(TOOLS_VENV)/bin
PY := $(BRIDLE_TOOLS_BIN)/python
BRIDLE_WHEEL ?=

$(TOOLS_VENV)/.stamp: scripts/requirements-dev.txt BRIDLE_VERSION
	@rm -rf $(TOOLS_VENV) && python3 -m venv $(TOOLS_VENV)
	@want=$$(sed -n 's/^wheel_sha256=//p' BRIDLE_VERSION); wheel="$(BRIDLE_WHEEL)"; \
	if [ -n "$$wheel" ]; then \
	  [ "$$(sha256sum "$$wheel" | cut -d' ' -f1)" = "$$want" ] || { echo "BRIDLE_WHEEL is not the wheel BRIDLE_VERSION names"; exit 1; }; \
	else \
	  for c in ../bridle/dist/bridle_protocol-*.whl; do \
	    [ -f "$$c" ] && [ "$$(sha256sum "$$c" | cut -d' ' -f1)" = "$$want" ] && wheel="$$c"; \
	  done; \
	fi; \
	if [ -n "$$wheel" ]; then \
	  grep -v '^bridle-protocol @' scripts/requirements-dev.txt | $(TOOLS_VENV)/bin/pip install -q -r /dev/stdin "$$wheel"; \
	else $(TOOLS_VENV)/bin/pip install -q -r scripts/requirements-dev.txt \
	  || { echo "could not fetch the pinned wheel; a private release needs BRIDLE_WHEEL=<wheel>"; exit 1; }; fi
	@touch $@

tools: $(TOOLS_VENV)/.stamp

# The committed headers and the wheel pin are the ones BRIDLE_VERSION names (no network).
bridle-check:
	@scripts/bridle-update.sh --check

clean-tools:
	rm -rf $(TOOLS_VENV)

# The recorded sessions the playback tests replay (tests/corpus/sessions/, README there), rendered into this
# repository's own namespace.
BRIDLE_GEN_DIR := build/bridle
BRIDLE_SESSION_FILES := $(sort $(wildcard tests/corpus/sessions/*.yaml))

$(BRIDLE_GEN_DIR)/sessions_generated.h: $(TOOLS_VENV)/.stamp $(BRIDLE_SESSION_FILES)
	@mkdir -p $(BRIDLE_GEN_DIR)
	@$(BRIDLE_TOOLS_BIN)/bridle-session gen-cpp $@ --namespace esphome::home_io_control::bridle $(BRIDLE_SESSION_FILES)

bridle-gen: $(BRIDLE_GEN_DIR)/sessions_generated.h

# --- Host unit-test build (incremental; objects mirror source paths) ---
# HOST_VARIANT selects the object/binary tree; "asan" adds sanitizer flags (see unit-test-asan).
HOST_VARIANT ?= default
HOST_BUILD_DIR := build/host/$(HOST_VARIANT)
HOST_OBJ_DIR := $(HOST_BUILD_DIR)/obj
HOST_EXTRA_FLAGS ?=
# -Wno-unused-variable / -Wno-unused-but-set-variable are deliberately ABSENT here — do not
# re-add them. tests/include/esphome/core/log.h's ESP_LOG* stubs name their arguments in a
# dead branch, so values only computed for logging still read as "used" on host. If a wave of
# unused-variable warnings ever reappears, that stub was broken — fix it there, not here.
# -Wno-unused-parameter stays: callback signatures conforming to an interface genuinely have
# unused parameters. -Wno-reorder stays: member-init-order patterns here rely on it.
# -std=c++20 matches the device build (ESP-IDF compiles this component as gnu++20), so the host
# tests exercise the sources under the same language rules the firmware does. They were on c++17,
# which meant C++20 library calls the device build accepts -- and that clang-tidy's device-side
# analysis actively recommends, e.g. modernize-use-starts-ends-with -- would not compile here.
# -Werror: every host warning fails the build, so warnings cannot pile up between clang-tidy runs.
# Safe because every CI job is pinned to ubuntu-26.04, whose g++ is the local dev toolchain's (15.2);
# a warning that appears only in CI means that pin drifted. It also makes -Wswitch hard: several
# switches over DeviceType / capability-class enums deliberately omit a default: label so an
# unhandled new enumerator is caught at build time rather than silently mapping to UNKNOWN, and the
# reverse cross-language sync tests (device_type_sync_test.cpp) rely on every enumerator being
# reachable through a named case.
HOST_CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-reorder -DIRAM_ATTR= \
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
# The default variant also collects the Bridle traces its stand-ins write (bridle-trace-check lints
# them); the sanitizer variant writes none.
BRIDLE_TRACE_DIR := $(CURDIR)/build/bridle/traces

host-run: $(HOST_TEST_BIN)
	@echo "Running tests ($(HOST_VARIANT))..."
	@if [ "$(HOST_VARIANT)" = default ]; then rm -rf $(BRIDLE_TRACE_DIR); fi
	@$(if $(filter default,$(HOST_VARIANT)),HIOC_BRIDLE_TRACE_DIR=$(BRIDLE_TRACE_DIR)) UBSAN_OPTIONS=print_stacktrace=1 \
		LSAN_OPTIONS=suppressions=$(CURDIR)/tests/asan/lsan.supp \
		./$(HOST_TEST_BIN)

# Public entry point: corpus-gen runs first (ordering!), then a parallel sub-make builds/links.
# Parallelism is scoped to this sub-make (not a global -j) so the docker/esphome targets in
# this Makefile stay serial. After changing HOST_CXXFLAGS, run `make clean-host` — make does
# not hash command lines, so flag-only changes don't invalidate existing objects.
unit-test: corpus-gen bridle-gen
	@echo "Building Google Test unit tests for home_io_control (host-only)..."
	@$(MAKE) --no-print-directory -j$(shell nproc) host-run

# ASan/UBSan variant: same sources and rules as unit-test, built into a fully separate object
# tree (build/host/asan/) so the two variants never poison each other. Uninstrumented system
# libgtest is fine to link against — ASan intercepts allocation globally.
ASAN_HOST_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -fno-omit-frame-pointer

unit-test-asan: corpus-gen bridle-gen
	@echo "Building Google Test unit tests for home_io_control (ASan/UBSan)..."
	@$(MAKE) --no-print-directory -j$(shell nproc) \
		HOST_VARIANT=asan HOST_EXTRA_FLAGS="$(ASAN_HOST_FLAGS)" host-run

# === Full-stack tests ===========================================================
# The hub's exchange engine over the real Bridle driver over a simulated co-processor, with an emulated
# 2.4 GHz blind on the simulated air (tests/fullstack/, tests/support/sim_bridge.h). The simulator is any
# program that speaks the simulator control channel of the Bridle specification (section 11.1) and has
# an 802.15.4 radio: BRIDLE_SIM names its binary. Its own object tree (build/host/fullstack/), so the
# simulator-dependent sources never reach the unit-test binary. fullstack-build only compiles the suite
# (no simulator needed), which keeps it building in CI.
BRIDLE_SIM ?=

fullstack-build: corpus-gen bridle-gen
	@echo "Building the full-stack tests..."
	@$(MAKE) --no-print-directory -j$(shell nproc) HOST_VARIANT=fullstack \
		HOST_SRCS="$(COMPONENT_SRCS) $(STUB_SRCS) $(FULLSTACK_SRCS)" fullstack-bin

# The binary of the sub-make's variant: HOST_TEST_BIN expands there, not in the caller.
fullstack-bin: $(HOST_TEST_BIN)

fullstack: fullstack-build
	@test -x "$(BRIDLE_SIM)" || { echo "fullstack: set BRIDLE_SIM= to the binary of a Bridle simulator (Bridle specification section 11.1)"; exit 1; }
	@echo "Running the full-stack tests..."
	@$(MAKE) --no-print-directory HOST_VARIANT=fullstack \
		HOST_SRCS="$(COMPONENT_SRCS) $(STUB_SRCS) $(FULLSTACK_SRCS)" fullstack-run

fullstack-run: $(HOST_TEST_BIN)
	@BRIDLE_SIM=$(abspath $(BRIDLE_SIM)) ./$(HOST_TEST_BIN)

# Regenerates the session fixtures from the host tests that record them; see the script's docstring
# for when to, and review the diff before committing.
bridle-sessions-regen: unit-test tools
	@$(PY) scripts/regen-bridle-sessions.py

# The session linter (Bridle specification section 12.1) over every trace the default unit-test run wrote:
# a driver that breaks a rule of the spec fails here even when its C++ test passed.
bridle-trace-check: unit-test tools
	@echo "Linting the Bridle traces of the unit tests..."
	@n=0; for f in $(BRIDLE_TRACE_DIR)/*.log; do \
	  [ -e "$$f" ] || { echo "no traces in $(BRIDLE_TRACE_DIR)"; exit 1; }; \
	  $(BRIDLE_TOOLS_BIN)/bridle-session check "$$f" || { echo "^ $$f"; exit 1; }; n=$$((n+1)); \
	done; echo "bridle-trace-check: OK ($$n traces)"

clean-host:
	rm -rf build/host

# Codegen behaviour tests (tests/python/run_tests.py): validators, ID injection, and `esphome config`
# over the accept/reject fixtures in tests/python/fixtures/. Runs inside the ESPHome image because
# the modules import esphome.*; the repo is mounted read-only so the run can never write into it.
py-test: tools
	@echo "Running codegen tests in the ESPHome container..."
	@docker compose run --rm -v "$(CURDIR):/repo:ro" -w /repo -e PYTHONDONTWRITEBYTECODE=1 \
		--entrypoint python3 esphome tests/python/run_tests.py
	@$(PY) scripts/bench/ref868_test.py
	@$(PY) scripts/bench/io2g4_test.py
	@$(PY) scripts/bench/respond_report_test.py


# === Documentation =============================================================

DOXYGEN_OUTPUT := docs/doxygen

doxygen:
	@echo "Generating Doxygen documentation..."
	@scripts/generate-doxygen.sh


# === Composite targets =========================================================

# Every sub-target of `lint` below must have a matching CI job in .github/workflows/ci.yml,
# so a local-only check can never silently drift out of CI coverage:
#   format-check        -> format
#   yamllint            -> yamllint
#   clang-tidy          -> tidy
#   tuning-sync         -> tuning-sync
#   yaml-emitter-sync   -> yaml-emitter-sync
#   include-graph       -> include-graph
#   stub-sync           -> stub-sync
#   board-pinout-sync   -> board-pinout-sync
#   corpus-validate     -> corpus-validate
#   bridle-check        -> bridle-check
#   docs-link-check     -> docs-link-check
#   key-material-scan   -> key-material-scan
lint: format-check yamllint clang-tidy tuning-sync yaml-emitter-sync include-graph stub-sync board-pinout-sync corpus-validate bridle-check docs-link-check docs-prose-check key-material-scan
test: unit-test bridle-trace-check unit-test-asan py-test firmware-test
check: lint test doxygen

# Backward compatibility aliases (deprecated, use new names)
test-compile: firmware-test
test-unit: unit-test


# === Phony declarations ========================================================

.PHONY: dashboard fullstack fullstack-build fullstack-bin fullstack-run \
		format format-check yamllint clang-tidy tidy tuning-sync corpus-validate corpus-gen \
		docs-link-check \
		include-graph stub-sync \
		docs-prose-check \
		key-material-scan \
		fuzz-frame fuzz-soft-phy fuzz-bridle-codec fuzz-io2g4-unwrap bridle-gen tools clean-tools bridle-check \
		firmware-test firmware-test-group-1 firmware-test-group-2 firmware-test-groups-check \
		unit-test unit-test-asan py-test host-run clean-host lint test check \
		test-compile test-unit \
		doxygen clean-docs clean-test-cache
