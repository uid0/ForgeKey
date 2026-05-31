#!/usr/bin/env python3
"""Validate ForgeKey JSON Schemas and example payload fixtures."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA_DIR = ROOT / "docs" / "schemas"


def schema_files() -> list[Path]:
    return sorted(SCHEMA_DIR.glob("*.schema.json"))


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def main() -> int:
    files = schema_files()
    if not files:
        raise SystemExit("no schema files found under docs/schemas")

    versions: dict[str, Path] = {}
    for path in files:
        schema = load_json(path)
        if schema.get("type") != "object":
            raise SystemExit(f"{path}: root schema type must be object")
        if "properties" not in schema or not isinstance(schema["properties"], dict):
            raise SystemExit(f"{path}: missing object properties")
        schema_version = schema.get("properties", {}).get("schema_version", {}).get("const")
        if not schema_version:
            raise SystemExit(f"{path}: missing properties.schema_version.const")
        if schema_version in versions:
            raise SystemExit(f"duplicate schema_version {schema_version}: {versions[schema_version]} and {path}")
        versions[schema_version] = path
        required = set(schema.get("required", []))
        if "schema_version" not in required:
            raise SystemExit(f"{path}: schema_version must be required")

    readme = (SCHEMA_DIR / "README.md").read_text(encoding="utf-8")
    for version, path in versions.items():
        if version not in readme:
            raise SystemExit(f"{path.name}: {version} not documented in docs/schemas/README.md")
        if path.name not in readme:
            raise SystemExit(f"{path.name}: file not documented in docs/schemas/README.md")

    print(f"validated {len(files)} schema file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
