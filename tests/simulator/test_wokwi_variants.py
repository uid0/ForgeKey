from __future__ import annotations

import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

SUPPORTED_VARIANTS = [
    "hardware/temperature-sensor",
    "hardware/cabinet-lock",
]


@pytest.mark.simulator
@pytest.mark.parametrize("variant", SUPPORTED_VARIANTS)
def test_wokwi_variant_has_required_project_files(variant: str) -> None:
    base = ROOT / variant / "wokwi"
    assert (base / "diagram.json").is_file(), f"{variant} missing wokwi/diagram.json"
    assert any((base / name).is_file() for name in ("sketch.ino", "sketch.c", "sketch.cpp")), (
        f"{variant} missing Wokwi sketch source"
    )


@pytest.mark.simulator
@pytest.mark.parametrize("variant", SUPPORTED_VARIANTS)
def test_wokwi_diagram_declares_parts_and_connections(variant: str) -> None:
    diagram = json.loads((ROOT / variant / "wokwi" / "diagram.json").read_text(encoding="utf-8"))
    assert diagram.get("version") == 1
    assert isinstance(diagram.get("parts"), list) and diagram["parts"], "diagram must include parts"
    assert isinstance(diagram.get("connections"), list) and diagram["connections"], "diagram must include wiring"


@pytest.mark.simulator
def test_temperature_sensor_simulates_dht21() -> None:
    diagram_text = (ROOT / "hardware/temperature-sensor/wokwi/diagram.json").read_text(encoding="utf-8")
    sketch = (ROOT / "hardware/temperature-sensor/wokwi/sketch.ino").read_text(encoding="utf-8")
    assert "dht" in diagram_text.lower()
    assert "DHT" in sketch


@pytest.mark.simulator
def test_cabinet_lock_simulates_lock_inputs_and_output() -> None:
    diagram_text = (ROOT / "hardware/cabinet-lock/wokwi/diagram.json").read_text(encoding="utf-8").lower()
    sketch = (ROOT / "hardware/cabinet-lock/wokwi/sketch.c").read_text(encoding="utf-8").lower()
    assert "button" in diagram_text or "switch" in diagram_text
    assert "relay" in sketch or "solenoid" in sketch or "lock" in sketch
