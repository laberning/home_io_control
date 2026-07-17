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
                    t3s3-lr1121:t3s3 \
                    t3s3-lr1121-monitor:t3s3-monitor

variant_stem = $(word 1,$(subst :, ,$1))
variant_suffix = $(word 2,$(subst :, ,$1))

$(foreach v,$(DEVICE_VARIANTS),$(eval compile-$(call variant_suffix,$(v)): ; $(call compile_recipe,$(call variant_stem,$(v)))))
$(foreach v,$(DEVICE_VARIANTS),$(eval upload-$(call variant_suffix,$(v)): ; $(call upload_recipe,$(call variant_stem,$(v)))))
$(foreach v,$(DEVICE_VARIANTS),$(eval logs-$(call variant_suffix,$(v)): ; $(call logs_recipe,$(call variant_stem,$(v)))))
$(foreach v,$(DEVICE_VARIANTS),$(eval clean-$(call variant_suffix,$(v)): ; $(call clean_recipe,$(call variant_stem,$(v)))))

# === Convenience defaults (delegate to v2) ====================================
compile: compile-v2
upload: upload-v2
logs: logs-v2
clean: clean-v2

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
	yamllint config/tests/ config/*.yaml tests/corpus/captures/ 2>/dev/null || exit 1

# Golden-frame corpus: schema + self-consistency validation of every capture YAML
# (CRC, CTRL0 length, duplicate ids), plus the ingest/build/validate tool self-tests.
# The generated C++ headers are used for unit-tests
corpus-validate:
	@echo "Validating golden-frame corpus captures..."
	@python3 scripts/corpus/validate.py
	@python3 scripts/corpus/tests/run_tests.py

# Static analysis (local clang-tidy after building inside Docker)
clang-tidy:
	@echo "Running clang-tidy..."
	@scripts/run-clang-tidy.sh

# Cross-language check: tuning parameter names must match between tuning.py and the C++ registry
tuning-sync:
	@echo "Checking tuning parameter sync (tuning.py <-> tuning_registry.cpp)..."
	@python3 scripts/check-tuning-sync.py

# Wipes config/tests/.esphome/build/<env>/ for every config/tests/test-*.yaml config (the ones
# make clang-tidy / firmware-test / check build against), plus config/tests/.esphome/storage/
# (per-config validated-YAML cache, keyed by config filename rather than device_name, so it's
# wiped in one shot instead of per-env). Uses Docker rather than a host-side rm — these paths are
# root-owned (created by a previous Docker run), so a plain `rm` on the host fails with Permission
# denied — including yamllint's read of storage/*.validated.yaml during `make lint`. Run this
# before make clang-tidy / firmware-test / check whenever a build directory might be stale or
# half-regenerated (see AGENTS.md), and also the first time in a session that you add a new .cpp
# under components/home_io_control/ — SCons sometimes fails to notice a newly added source file in
# a pre-existing build directory, producing a confusing "undefined reference to vtable" linker
# error even though the file compiles fine under make unit-test.
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

# Compilation tests for all platform configs
firmware-test:
	@echo "Compiling test configurations in config/tests/"
	@for cfg in config/tests/test-*.yaml; do \
	  name=$$(basename "$$cfg"); \
	  echo "=== Compiling $$name ==="; \
	  docker compose run --rm esphome compile "/config/tests/$$name" || exit 1; \
	done

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
UNIT_TEST_DEFINES := -DUSE_API_USER_DEFINED_ACTIONS \
			 -DUSE_API_CUSTOM_SERVICES \
			 -DUSE_API_HOMEASSISTANT_SERVICES

# Regenerates build/corpus/corpus_generated.h from tests/corpus/captures/**/*.yaml.
# see tests/corpus/README.md.
corpus-gen:
	@mkdir -p build
	@python3 scripts/corpus/build.py

unit-test: corpus-gen
	@echo "Building Google Test unit tests for home_io_control (host-only)..."
	@mkdir -p build
	g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-variable -Wno-reorder -DIRAM_ATTR= \
		$(UNIT_TEST_DEFINES) $(INCLUDES) \
		$(COMPONENT_SRCS) $(STUB_SRCS) $(TEST_SRCS) \
		-lgtest -lgtest_main -pthread \
		-o build/test_home_io_control
	@echo "Linking complete. Running tests..."
	@./build/test_home_io_control


# === Documentation =============================================================

DOXYGEN_OUTPUT := docs/doxygen

doxygen:
	@echo "Generating Doxygen documentation..."
	@scripts/generate-doxygen.sh


# === Composite targets =========================================================

lint: format-check yamllint clang-tidy tuning-sync corpus-validate
test: unit-test firmware-test
check: lint test doxygen

# Backward compatibility aliases (deprecated, use new names)
test-compile: firmware-test
test-unit: unit-test


# === Phony declarations ========================================================

.PHONY: dashboard \
		format format-check yamllint clang-tidy tidy tuning-sync corpus-validate corpus-gen \
		firmware-test unit-test lint test check \
		test-compile test-unit \
		doxygen clean-docs clean-test-cache
