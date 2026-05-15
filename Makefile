# === Makefile for Home IO Control ===============================================
# Default goal: compile firmware for the default device (v2 / sx1276)
.DEFAULT_GOAL := compile


# === Recipe fragments (shared) ==========================================================
# $1 is the device stem (e.g., v2, v3-monitor)
define compile_recipe
	@file="heltec-wifi-lora-32-$1.yaml"; \
	echo "Compiling $$$$file"; \
	docker compose run --rm esphome compile "$$$$file"
endef

define upload_recipe
	@file="heltec-wifi-lora-32-$1.yaml"; \
	echo "Uploading to $$$$file"; \
	docker compose run --rm esphome run "$$$$file"
endef

define logs_recipe
	@file="heltec-wifi-lora-32-$1.yaml"; \
	echo "Streaming logs from $$$$file"; \
	docker compose run --rm esphome logs "$$$$file"
endef

define clean_recipe
	@file="heltec-wifi-lora-32-$1.yaml"; \
	echo "Cleaning build artifacts for $$$$file"; \
	docker compose run --rm esphome clean "$$$$file"
endef

# === Explicit device-variant targets (for tab completion & direct invocation) ===
DEVICE_VARIANTS := v2 v3 v3-monitor

$(foreach v,$(DEVICE_VARIANTS),$(eval compile-$(v): ; $(call compile_recipe,$(v))))
$(foreach v,$(DEVICE_VARIANTS),$(eval upload-$(v): ; $(call upload_recipe,$(v))))
$(foreach v,$(DEVICE_VARIANTS),$(eval logs-$(v): ; $(call logs_recipe,$(v))))
$(foreach v,$(DEVICE_VARIANTS),$(eval clean-$(v): ; $(call clean_recipe,$(v))))

# === Convenience defaults (delegate to v2) ====================================
compile: compile-v2
upload: upload-v2
logs: logs-v2
clean: clean-v2


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
	yamllint config/tests/ config/*.yaml 2>/dev/null || exit 1

# Static analysis (local clang-tidy after building inside Docker)
clang-tidy:
	@echo "Running clang-tidy..."
	@scripts/run-clang-tidy.sh

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

# Include paths
INCLUDES := -Icomponents/home_io_control \
            -Itests/include \
            -Itests/support

unit-test:
	@echo "Building Google Test unit tests for home_io_control (host-only)..."
	@mkdir -p build
	g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-variable -Wno-reorder -fno-access-control -DIRAM_ATTR= \
		$(INCLUDES) \
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

lint: format-check yamllint clang-tidy
test: unit-test firmware-test
check: lint test

# Backward compatibility aliases (deprecated, use new names)
test-compile: firmware-test
test-unit: unit-test


# === Phony declarations ========================================================

.PHONY: dashboard \
		format format-check yamllint clang-tidy tidy \
		firmware-test unit-test lint test check \
		test-compile test-unit \
		doxygen
