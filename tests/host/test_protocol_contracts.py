from __future__ import annotations

import base64
import hashlib
import json
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_DIR = ROOT / "docs" / "schemas"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


# Indicator contract sets — the documented set_indicator contract (the shared
# SSOT in docs/contracts/mqtt.md), mirroring the firmware keyword/color/pattern
# tables in src/capabilities/status_matrix/status_matrix.cpp. Keep these in sync
# with that doc and those resolvers. (The firmware accepts a lenient superset of
# this contract — e.g. it clamps out-of-range numeric values rather than
# rejecting them — so these checks assert the contract boundary, not byte-for-
# byte firmware leniency.)
INDICATOR_KEYWORDS = {
    # base + existing aliases
    "ok", "available", "green",
    "attention", "warning", "yellow",
    "error", "critical", "red",
    "busy", "reserved", "blue",
    "off", "auto", "clear",
    # new color keyword + status aliases
    "purple", "magenta",
    "in_use", "unavailable", "classroom", "class", "locked_out",
}
COLOR_NAMES = {
    "red", "green", "blue", "yellow", "orange",
    "purple", "magenta", "pink", "cyan", "white", "off", "black",
}
PATTERN_MODES = {"solid", "blink", "slow_blink", "breathe", "off"}
BRIGHTNESS_WORDS = {"low", "high"}
_HEX_COLOR = re.compile(r"^#?[0-9a-fA-F]{6}$")


def _validate_indicator_color(value: object) -> None:
    if isinstance(value, list):
        rgb = value[:3]
        if len(value) < 3 or not all(isinstance(c, int) and not isinstance(c, bool) and 0 <= c <= 255 for c in rgb):
            raise ValueError("color array must be [r,g,b] with 0-255 ints")
        return
    if isinstance(value, str):
        if value.lower() in COLOR_NAMES or _HEX_COLOR.match(value):
            return
        raise ValueError(f"unsupported color {value!r}")
    raise ValueError("color must be a name, #RRGGBB, or [r,g,b]")


def _validate_indicator_brightness(value: object) -> None:
    if isinstance(value, bool):
        raise ValueError("brightness must be a word or 0-255 int")
    if isinstance(value, int):
        if not 0 <= value <= 255:
            raise ValueError("brightness out of range")
        return
    if isinstance(value, str):
        if value.lower() in BRIGHTNESS_WORDS:
            return
        raise ValueError(f"unsupported brightness {value!r}")
    raise ValueError("brightness must be a word or 0-255 int")


def validate_indicator_command(doc: dict) -> None:
    """Validate a set_indicator/set_pattern command against the contract.

    Explicit color/brightness/pattern override the keyword defaults; values
    outside the documented contract are rejected; the command must carry at
    least one presentation directive (keyword, color, or pattern). The firmware
    implements a lenient superset (it clamps out-of-range numerics), so this is
    the strict contract boundary OMS should target, not a behavioral mirror.
    """
    keyword = doc.get("indicator") or doc.get("state")
    have_keyword = keyword is not None
    if have_keyword and keyword not in INDICATOR_KEYWORDS:
        raise ValueError(f"unsupported indicator {keyword!r}")

    have_color = "color" in doc and doc["color"] is not None
    if have_color:
        _validate_indicator_color(doc["color"])

    if doc.get("brightness") is not None:
        _validate_indicator_brightness(doc["brightness"])

    pattern = doc.get("pattern")
    have_pattern = isinstance(pattern, str) and pattern != ""
    if have_pattern:
        if pattern in PATTERN_MODES:
            pass
        elif not have_keyword and not have_color:
            # legacy set_pattern carried the semantic keyword in `pattern`
            if pattern not in INDICATOR_KEYWORDS:
                raise ValueError(f"unsupported pattern {pattern!r}")
            have_keyword = True
        else:
            raise ValueError(f"unsupported pattern {pattern!r}")

    if "period_ms" in doc and not (isinstance(doc["period_ms"], int) and not isinstance(doc["period_ms"], bool)):
        raise ValueError("period_ms must be an integer")

    if not (have_keyword or have_color or have_pattern):
        raise ValueError("indicator command needs indicator/color/pattern")


def parse_command(payload: bytes) -> dict:
    doc = json.loads(payload)
    if not isinstance(doc, dict):
        raise ValueError("command payload must be an object")
    if doc.get("schema_version") != "forgekey.command.v1":
        raise ValueError("unsupported command schema")
    cmd = doc.get("cmd")
    if not isinstance(cmd, str) or not cmd.strip():
        raise ValueError("missing cmd")
    if cmd in {"blink", "identify"}:
        duration = int(doc.get("duration_s", 30))
        if not 1 <= duration <= 300:
            raise ValueError("duration_s out of range")
    elif cmd in {"set_indicator", "set_pattern"}:
        validate_indicator_command(doc)
    elif cmd in {"ota", "update_firmware"}:
        if not isinstance(doc.get("url"), str) and not isinstance(doc.get("policy"), dict):
            raise ValueError("ota command missing url or policy")
    elif cmd not in {"reboot", "status", "config"}:
        raise ValueError(f"unsupported command {cmd}")
    return doc


def validate_ota_payload(payload: dict) -> dict:
    policy = payload.get("policy", payload)
    url = policy.get("url") or payload.get("url")
    sha256 = (policy.get("sha256") or payload.get("sha256") or "").lower()
    signature = policy.get("signature") or payload.get("signature")
    if not isinstance(url, str) or not url.startswith("https://"):
        raise ValueError("OTA url must be HTTPS")
    if not re.fullmatch(r"[0-9a-f]{64}", sha256):
        raise ValueError("sha256 must be 64 lowercase hex characters")
    if not signature:
        raise ValueError("signature is required")
    return {"url": url, "sha256": sha256, "signature": signature}


def canonical_manifest_bytes(manifest: dict) -> bytes:
    return json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()


def sign_manifest_with_openssl(tmp_path: Path, manifest: dict) -> tuple[Path, bytes]:
    private_key = tmp_path / "manifest.key.pem"
    public_key = tmp_path / "manifest.pub.pem"
    manifest_path = tmp_path / "manifest.json"
    signature_path = tmp_path / "manifest.sig"
    manifest_path.write_bytes(canonical_manifest_bytes(manifest))
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", private_key], check=True)
    subprocess.run(["openssl", "ec", "-in", private_key, "-pubout", "-out", public_key], check=True, capture_output=True)
    subprocess.run(["openssl", "dgst", "-sha256", "-sign", private_key, "-out", signature_path, manifest_path], check=True)
    return public_key, signature_path.read_bytes()


def verify_manifest_signature(public_key: Path, tmp_path: Path, manifest: dict, signature: bytes) -> bool:
    manifest_path = tmp_path / "verify-manifest.json"
    signature_path = tmp_path / "verify-manifest.sig"
    manifest_path.write_bytes(canonical_manifest_bytes(manifest))
    signature_path.write_bytes(signature)
    result = subprocess.run(
        ["openssl", "dgst", "-sha256", "-verify", public_key, "-signature", signature_path, manifest_path],
        text=True,
        capture_output=True,
    )
    return result.returncode == 0


@dataclass
class DesiredConfig:
    wifi_networks: list[dict] = field(default_factory=list)
    ble_enabled: bool = True
    scan_interval_ms: int = 60_000
    identity_mode: str = "hashed"


def reconcile_config(current: DesiredConfig, desired: dict) -> tuple[DesiredConfig, list[str]]:
    changed: list[str] = []
    next_config = DesiredConfig(
        wifi_networks=list(current.wifi_networks),
        ble_enabled=current.ble_enabled,
        scan_interval_ms=current.scan_interval_ms,
        identity_mode=current.identity_mode,
    )
    if "wifi" in desired:
        networks = desired["wifi"].get("networks", [])
        if not isinstance(networks, list) or any("ssid" not in n for n in networks):
            raise ValueError("wifi.networks entries require ssid")
        next_config.wifi_networks = networks[:2]
        changed.append("wifi")
    if "ble" in desired:
        ble = desired["ble"]
        next_config.ble_enabled = bool(ble.get("enabled", next_config.ble_enabled))
        next_config.scan_interval_ms = max(10_000, int(ble.get("scan_interval_ms", next_config.scan_interval_ms)))
        identity_mode = ble.get("identity_mode", next_config.identity_mode)
        if identity_mode not in {"hashed", "ephemeral"}:
            raise ValueError("invalid_identity_mode")
        next_config.identity_mode = identity_mode
        changed.append("ble")
    return next_config, changed


def load_schema(name: str) -> dict:
    with (SCHEMA_DIR / name).open(encoding="utf-8") as fh:
        return json.load(fh)


def assert_required_fields(schema: dict, payload: dict) -> None:
    for field_name in schema.get("required", []):
        assert field_name in payload
    expected_schema_version = schema.get("properties", {}).get("schema_version", {}).get("const")
    if expected_schema_version:
        assert payload.get("schema_version") == expected_schema_version


def test_command_parsing_accepts_supported_identify_command() -> None:
    doc = parse_command(b'{"schema_version":"forgekey.command.v1","cmd":"identify","duration_s":45}')
    assert doc["cmd"] == "identify"


def test_command_parsing_accepts_indicator_command() -> None:
    doc = parse_command(b'{"schema_version":"forgekey.command.v1","cmd":"set_indicator","indicator":"attention"}')
    assert doc["indicator"] == "attention"


@pytest.mark.parametrize(
    "payload",
    [
        b"{}",
        b'{"schema_version":"forgekey.command.v1","cmd":"identify","duration_s":0}',
        b'{"schema_version":"forgekey.command.v1","cmd":"set_indicator","indicator":"sparkle"}',
        b'{"schema_version":"forgekey.command.v1","cmd":"unlock_forever"}',
    ],
)
def test_command_parsing_rejects_invalid_or_unsupported_commands(payload: bytes) -> None:
    with pytest.raises(ValueError):
        parse_command(payload)


def _indicator_payload(**fields: object) -> bytes:
    body = {"schema_version": "forgekey.command.v1", "cmd": "set_indicator", **fields}
    return json.dumps(body).encode()


@pytest.mark.parametrize(
    "fields",
    [
        # Canonical presentation payloads OMS sends (epic spec).
        {"color": "green", "brightness": "low", "pattern": "solid"},   # available
        {"color": "green", "brightness": "high", "pattern": "solid"},  # in use
        {"color": "red", "brightness": "low", "pattern": "solid"},     # unavailable
        {"pattern": "off"},                                            # locked out
        {"color": "purple", "brightness": "high", "pattern": "slow_blink", "period_ms": 1500},  # class
        # Color override forms: named purple, hex, and [r,g,b].
        {"color": "purple"},
        {"color": "#800080"},
        {"color": [128, 0, 255]},
        {"color": "magenta", "brightness": 200},
        # Brightness words and integer.
        {"color": "green", "brightness": "low"},
        {"color": "green", "brightness": "high"},
        {"color": "green", "brightness": 12},
        # Pattern modes.
        {"color": "blue", "pattern": "breathe"},
        {"indicator": "error", "pattern": "blink"},
        # Bare keywords: back-compat base + new status aliases.
        {"indicator": "ok"},
        {"indicator": "attention", "duration_s": 30},
        {"indicator": "in_use"},
        {"indicator": "unavailable"},
        {"indicator": "classroom"},
        {"indicator": "class"},
        {"indicator": "locked_out"},
        {"indicator": "purple"},
        {"indicator": "auto"},
    ],
)
def test_indicator_command_accepts_extended_presentation(fields: dict) -> None:
    doc = parse_command(_indicator_payload(**fields))
    assert doc["cmd"] == "set_indicator"


def test_set_pattern_alias_accepts_legacy_keyword_in_pattern_field() -> None:
    payload = b'{"schema_version":"forgekey.command.v1","cmd":"set_pattern","pattern":"ok"}'
    assert parse_command(payload)["cmd"] == "set_pattern"


@pytest.mark.parametrize(
    "fields",
    [
        {"color": "chartreuse"},               # unknown named color
        {"color": "#12"},                      # malformed hex
        {"color": [0, 0]},                     # too few components
        {"color": [0, 0, 300]},                # component out of range
        {"brightness": "dim"},                 # unknown brightness word
        {"brightness": 999},                   # brightness out of range
        {"color": "green", "pattern": "strobe"},  # unknown pattern with explicit color
        {"indicator": "sparkle"},              # unknown keyword
        {},                                    # no presentation directive at all
    ],
)
def test_indicator_command_rejects_unsupported_presentation(fields: dict) -> None:
    with pytest.raises(ValueError):
        parse_command(_indicator_payload(**fields))


def test_ota_payload_validation_requires_https_sha256_and_signature() -> None:
    valid = validate_ota_payload(
        {
            "policy": {
                "url": "https://oms.example/fw.bin",
                "sha256": hashlib.sha256(b"firmware").hexdigest(),
                "signature": "sig",
            }
        }
    )
    assert valid["url"].startswith("https://")
    assert valid["sha256"] == hashlib.sha256(b"firmware").hexdigest()

    with pytest.raises(ValueError, match="HTTPS"):
        validate_ota_payload({"url": "http://oms.example/fw.bin", "sha256": "0" * 64, "signature": "sig"})
    with pytest.raises(ValueError, match="sha256"):
        validate_ota_payload({"url": "https://oms.example/fw.bin", "sha256": "bad", "signature": "sig"})
    with pytest.raises(ValueError, match="signature"):
        validate_ota_payload({"url": "https://oms.example/fw.bin", "sha256": "0" * 64})


def test_signature_verification_accepts_signed_manifest_and_rejects_tampering(tmp_path: Path) -> None:
    manifest = {"version": "1.2.3", "sha256": "a" * 64, "url": "https://oms.example/fw.bin"}
    public_key, signature = sign_manifest_with_openssl(tmp_path, manifest)
    assert verify_manifest_signature(public_key, tmp_path, manifest, signature)
    tampered = {**manifest, "version": "1.2.4"}
    assert not verify_manifest_signature(public_key, tmp_path, tampered, signature)


def test_config_reconciliation_clamps_ble_and_limits_wifi_networks() -> None:
    current = DesiredConfig()
    desired = {
        "wifi": {"networks": [{"ssid": "primary"}, {"ssid": "backup"}, {"ssid": "ignored"}]},
        "ble": {"enabled": False, "scan_interval_ms": 250, "identity_mode": "ephemeral"},
    }
    reconciled, changed = reconcile_config(current, desired)
    assert changed == ["wifi", "ble"]
    assert [n["ssid"] for n in reconciled.wifi_networks] == ["primary", "backup"]
    assert reconciled.scan_interval_ms == 10_000
    assert reconciled.identity_mode == "ephemeral"


def test_schema_generation_contracts_are_valid_and_documented() -> None:
    readme = (SCHEMA_DIR / "README.md").read_text(encoding="utf-8")
    seen: set[str] = set()
    for path in sorted(SCHEMA_DIR.glob("*.schema.json")):
        schema = load_schema(path.name)
        assert schema.get("type") == "object"
        version = schema["properties"]["schema_version"]["const"]
        assert version not in seen
        seen.add(version)
        assert "schema_version" in schema.get("required", [])
        assert version in readme
        assert path.name in readme


def test_telemetry_serialization_matches_status_schema() -> None:
    payload = {
        "schema_version": "forgekey.status.v1",
        "online": True,
        "ip": "192.0.2.10",
        "firmware_version": "test",
        "build": {"target": "host"},
    }
    schema = load_schema("status.v1.schema.json")
    assert_required_fields(schema, payload)
    serialized = json.dumps(payload, separators=(",", ":"), sort_keys=True)
    restored = json.loads(serialized)
    assert restored == payload
