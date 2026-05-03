compile:
	docker compose run --rm esphome compile heltec_wifi_lora_32_v2.yaml

upload:
	docker compose run --rm esphome run heltec_wifi_lora_32_v2.yaml

logs:
	docker compose run --rm esphome logs heltec_wifi_lora_32_v2.yaml

clean:
	docker compose run --rm esphome clean heltec_wifi_lora_32_v2.yaml

compile-v3:
	docker compose run --rm esphome compile heltec_wifi_lora_32_v3.yaml

upload-v3:
	docker compose run --rm esphome run heltec_wifi_lora_32_v3.yaml

logs-v3:
	docker compose run --rm esphome logs heltec_wifi_lora_32_v3.yaml

clean-v3:
	docker compose run --rm esphome clean heltec_wifi_lora_32_v3.yaml

compile-v3-monitor:
	docker compose run --rm esphome compile heltec_wifi_lora_32_v3_monitor.yaml

upload-v3-monitor:
	docker compose run --rm esphome run heltec_wifi_lora_32_v3_monitor.yaml

logs-v3-monitor:
	docker compose run --rm esphome logs heltec_wifi_lora_32_v3_monitor.yaml

clean-v3-monitor:
	docker compose run --rm esphome clean heltec_wifi_lora_32_v3_monitor.yaml

dashboard:
	docker compose up

# --- QA targets --------------------------------------------------------------

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
	@echo "Running clang-tidy via helper script..."
	@./scripts/run-clang-tidy.sh

# Compilation tests for all platform configs
firmware-test:
	@echo "Compiling test configurations in config/tests/"
	@for cfg in config/tests/test.*.yaml; do \
	  name=$$(basename "$$cfg"); \
	  echo "=== Compiling $$name ==="; \
	  docker compose run --rm esphome compile "/config/tests/$$name" || exit 1; \
	done

# --- Unit test configuration ------------------------------------------------

# All component source files needed for tests (protocol + platform + hub)
# Exclude radio drivers — we provide stubs in tests/stubs/
COMPONENT_SRCS := $(wildcard components/home_io_control/*.cpp)
COMPONENT_SRCS := $(filter-out components/home_io_control/radio_%.cpp,$(COMPONENT_SRCS))

# Test stubs (minimal implementations for host-side unit tests)
STUB_SRCS := $(wildcard tests/stubs/*.cpp)

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

# Composite targets
lint: format-check yamllint clang-tidy
test: unit-test firmware-test
check: lint test

# Backward compatibility aliases (deprecated, use new names)
test-compile: firmware-test
test-unit: unit-test

.PHONY: compile upload logs clean compile-v3 upload-v3 logs-v3 clean-v3 \
		compile-v3-monitor upload-v3-monitor logs-v3-monitor clean-v3-monitor dashboard \
		format format-check yamllint clang-tidy tidy \
		firmware-test unit-test lint test check \
		test-compile test-unit
