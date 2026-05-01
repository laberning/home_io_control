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
	find components tests -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +

format-check:
	find components tests -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format --dry-run --Werror {} +

# YAML linting (safe selection, excludes generated .esphome)
yamllint:
	yamllint config/tests/ config/*.yaml 2>/dev/null || exit 1

# Static analysis (local clang-tidy after building inside Docker)
tidy:
	@echo "Building inside Docker if needed, then running local clang-tidy..."
	@./scripts/run-clang-tidy.sh

# Compilation tests for all platform configs
test-compile:
	@echo "Compiling test configurations in config/tests/"
	@for cfg in config/tests/test.*.yaml; do \
	  name=$$(basename "$$cfg"); \
	  echo "=== Compiling $$name ==="; \
	  docker compose run --rm esphome compile "/config/tests/$$name" || exit 1; \
	done

# Host-based unit tests (Google Test)
#test-unit:
#	./script/cpp_unit_test.py home_io_control

# Original host test (still works)
test-host:
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Icomponents/home_io_control -Itests/host/include \
		components/home_io_control/proto_frame.cpp \
		components/home_io_control/proto_commands.cpp \
		components/home_io_control/proto_crypto.cpp \
		tests/host/protocol_smoke.cpp \
		-o build/protocol_smoke
	./build/protocol_smoke

# Composite targets
lint: format-check yamllint tidy
test: test-host test-compile #test-unit
check: lint test

.PHONY: compile upload logs clean compile-v3 upload-v3 logs-v3 clean-v3 \
		compile-v3-monitor upload-v3-monitor logs-v3-monitor clean-v3-monitor dashboard \
		format format-check yamllint tidy test-compile test-unit test-host lint test check
