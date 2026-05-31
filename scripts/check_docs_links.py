#!/usr/bin/env python3
"""Check local Markdown links without requiring network access."""
from __future__ import annotations

import re
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
LINK_RE = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")


def iter_markdown() -> list[Path]:
    ignored = {".git", ".pio", "node_modules", "__pycache__"}
    return sorted(p for p in ROOT.rglob("*.md") if not any(part in ignored for part in p.parts))


def normalize_target(raw: str) -> str | None:
    target = raw.strip().split()[0].strip("<>")
    parsed = urlparse(target)
    if parsed.scheme in {"http", "https", "mailto"}:
        return None
    if target.startswith("#"):
        return None
    return unquote(target.split("#", 1)[0])


def main() -> int:
    failures: list[str] = []
    for md in iter_markdown():
        text = md.read_text(encoding="utf-8", errors="ignore")
        for lineno, match in enumerate(text.splitlines(), start=1):
            for raw in LINK_RE.findall(match):
                target = normalize_target(raw)
                if not target:
                    continue
                resolved = (md.parent / target).resolve()
                try:
                    resolved.relative_to(ROOT)
                except ValueError:
                    failures.append(f"{md.relative_to(ROOT)}:{lineno}: link escapes repo: {raw}")
                    continue
                if not resolved.exists():
                    failures.append(f"{md.relative_to(ROOT)}:{lineno}: missing local link: {raw}")
    if failures:
        print("\n".join(failures))
        return 1
    print(f"checked {len(iter_markdown())} markdown file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
