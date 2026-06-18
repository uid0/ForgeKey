SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

PYTHON ?= python
PIO ?= pio
IDF_PY ?= idf.py
IDF_EXPORT ?= $(IDF_PATH)/export.sh
HIL_EVIDENCE ?=

.PHONY: help install-dev test test-host test-simulator test-hil schemas docs-check verify-host \
        pio-build pio-people pio-temperature pio-epaper lock-build verify clean

help: ## Show common ForgeKey development targets.
	@awk 'BEGIN {FS = ":.*##"; printf "ForgeKey Make targets:\n"} /^[a-zA-Z0-9_.-]+:.*##/ {printf "  %-18s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

install-dev: ## Install host-side development/test dependencies.
	$(PYTHON) -m pip install -r requirements-dev.txt

test: test-host ## Alias for host tests that do not require hardware.

test-host: ## Run host-side tests excluding hardware-in-the-loop checks.
	$(PYTHON) -m pytest -m "not hil"

test-simulator: ## Run Wokwi simulator contract tests.
	$(PYTHON) -m pytest tests/simulator

test-hil: ## Run hardware-in-the-loop tests; set HIL_EVIDENCE=/path/to/evidence.json.
	@if [[ -z "$(HIL_EVIDENCE)" ]]; then \
		echo "HIL_EVIDENCE is required, e.g. make test-hil HIL_EVIDENCE=/path/to/smoke_evidence.json" >&2; \
		exit 2; \
	fi
	FORGEKEY_HIL_EVIDENCE="$(HIL_EVIDENCE)" $(PYTHON) -m pytest -m hil

schemas: ## Validate JSON Schema contracts.
	$(PYTHON) scripts/validate_schemas.py

docs-check: ## Validate local Markdown links.
	$(PYTHON) scripts/check_docs_links.py

verify-host: test-host schemas docs-check ## Run all host checks that do not require firmware toolchains.

pio-build: ## Build all PlatformIO environments.
	$(PIO) run

pio-people: ## Build the Arduino people-counter firmware.
	$(PIO) run -e seeed_xiao_esp32s3

pio-temperature: ## Build the Arduino temperature-sensor firmware.
	$(PIO) run -e seeed_xiao_esp32s3_temperature

pio-epaper: ## Build the Arduino ePaper-display firmware.
	$(PIO) run -e seeed_xiao_epaper

lock-build: ## Build the ESP-IDF cabinet-lock firmware.
	@if [[ ! -f "$(IDF_EXPORT)" ]]; then \
		echo "IDF export script not found at $(IDF_EXPORT). Set IDF_PATH or run ESP-IDF setup first." >&2; \
		exit 2; \
	fi
	cd esp32c6-lock && . "$(IDF_EXPORT)" && $(IDF_PY) build

verify: verify-host pio-build lock-build ## Run host checks plus PlatformIO and ESP-IDF firmware builds.

clean: ## Remove common generated caches and firmware build outputs.
	rm -rf .pytest_cache tests/*/__pycache__
	$(PIO) run -t clean || true
