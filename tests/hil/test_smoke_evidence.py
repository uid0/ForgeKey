from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
ROOT = Path(__file__).resolve().parents[2]
SCHEMA_DIR = ROOT / "docs" / "schemas"
REQUIRED_CHECKS = [
    "camera",
    "dht21",
    "epaper",
    "lock_sensors",
    "mqtt",
    "wifi_provisioning",
    "ota",
]


def _evidence_path() -> Path | None:
    raw = os.environ.get("FORGEKEY_HIL_EVIDENCE")
    if not raw:
        return None
    return Path(raw)


@pytest.mark.hil
def test_hil_smoke_evidence_covers_required_devices() -> None:
    path = _evidence_path()
    if path is None:
        pytest.skip("set FORGEKEY_HIL_EVIDENCE=/path/to/smoke_evidence.json to run HIL smoke tests")
    evidence = json.loads(path.read_text(encoding="utf-8"))
    checks = evidence.get("checks", {})
    missing = [name for name in REQUIRED_CHECKS if checks.get(name, {}).get("status") != "pass"]
    assert not missing, f"missing/pending HIL checks: {missing}"


@pytest.mark.hil
def test_hil_smoke_evidence_contains_schema_valid_telemetry_samples() -> None:
    path = _evidence_path()
    if path is None:
        pytest.skip("set FORGEKEY_HIL_EVIDENCE=/path/to/smoke_evidence.json to run HIL smoke tests")
    evidence = json.loads(path.read_text(encoding="utf-8"))
    samples = evidence.get("telemetry_samples", [])
    assert samples, "include at least one telemetry sample in smoke evidence"
    schemas = {
        schema["properties"]["schema_version"]["const"]: schema
        for schema in (json.loads(path.read_text(encoding="utf-8")) for path in SCHEMA_DIR.glob("*.schema.json"))
    }
    for sample in samples:
        version = sample.get("schema_version")
        assert version in schemas, f"unknown schema_version {version}"
        for required in schemas[version].get("required", []):
            assert required in sample, f"{version} sample missing required field {required}"
