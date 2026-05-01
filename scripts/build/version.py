"""PlatformIO extra script: version injection + artifact export.

Injects build-time identity into the firmware and copies the produced
`firmware.bin` to a versioned artifact path that staff can hand to OMS for
OTA dispatch.

Wire-up: in `platformio.ini`, add:

    extra_scripts =
        pre:scripts/build/version.py
        post:scripts/build/version.py

The same script handles both the pre-build flag injection and the post-build
artifact export; PlatformIO calls it twice with different `env` state.

Build identity sources (each is overridable via the corresponding env var):

* `FORGEKEY_FIRMWARE_VERSION`  - parsed from `src/provisioning/device_config.h`
                                  default, or env override. Reported to OMS at
                                  registration and used in the artifact name.
* `FIRMWARE_GIT_COMMIT`        - short SHA from `git rev-parse --short HEAD`,
                                  with `-dirty` suffix if the tree has uncommitted
                                  changes. Falls back to `unknown`.
* `FIRMWARE_BUILD_TIMESTAMP`   - Unix epoch seconds at build time. `SOURCE_DATE_EPOCH`
                                  is honored for reproducible builds.

Artifact layout (after a successful build):

    artifacts/
      forgekey-0.2.0-abcd123.bin
      forgekey-0.2.0-abcd123.bin.sha256
      forgekey-latest.bin            (symlink/copy)

The sha256 file is plain hex on a single line — same shape OMS feeds back in
the OTA dispatch payload, so staff can paste it verbatim.
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO at script time)


PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
DEVICE_CONFIG = PROJECT_DIR / "src" / "provisioning" / "device_config.h"
ARTIFACT_DIR = PROJECT_DIR / "artifacts"


def _run(cmd):
    try:
        return subprocess.check_output(
            cmd, cwd=str(PROJECT_DIR), stderr=subprocess.DEVNULL
        ).decode().strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def _firmware_version():
    # Env override wins so CI can stamp release tags without editing the header.
    override = os.environ.get("FORGEKEY_FIRMWARE_VERSION")
    if override:
        return override
    if not DEVICE_CONFIG.is_file():
        return "0.0.0"
    text = DEVICE_CONFIG.read_text()
    match = re.search(
        r'#define\s+FORGEKEY_FIRMWARE_VERSION\s+"([^"]+)"', text
    )
    return match.group(1) if match else "0.0.0"


def _git_commit():
    override = os.environ.get("FIRMWARE_GIT_COMMIT")
    if override:
        return override
    sha = _run(["git", "rev-parse", "--short=7", "HEAD"]) or "unknown"
    if sha != "unknown":
        dirty = _run(["git", "status", "--porcelain"])
        if dirty:
            sha += "-dirty"
    return sha


def _build_timestamp():
    # Honour SOURCE_DATE_EPOCH for reproducible builds when a downstream
    # packager sets it. Otherwise stamp wall-clock.
    sde = os.environ.get("SOURCE_DATE_EPOCH")
    if sde and sde.isdigit():
        return int(sde)
    return int(time.time())


VERSION = _firmware_version()
COMMIT = _git_commit()
BUILD_TS = _build_timestamp()


def _inject_flags():
    # CPPDEFINES is the canonical place for -D flags in PlatformIO. Quoted
    # strings need the embedded escaped quotes shape `\\"value\\"` so the
    # compiler sees a string literal.
    env.Append(CPPDEFINES=[  # noqa: F821
        ("FIRMWARE_BUILD_TIMESTAMP", str(BUILD_TS)),
        ("FIRMWARE_GIT_COMMIT", env.StringifyMacro(COMMIT)),  # noqa: F821
        ("FORGEKEY_FIRMWARE_VERSION", env.StringifyMacro(VERSION)),  # noqa: F821
    ])
    print(f"[version] FORGEKEY_FIRMWARE_VERSION={VERSION} "
          f"FIRMWARE_GIT_COMMIT={COMMIT} FIRMWARE_BUILD_TIMESTAMP={BUILD_TS}")


def _export_artifact(source, target, env):  # noqa: ARG001 (PIO callback signature)
    src = Path(str(target[0]))
    if not src.is_file():
        print(f"[version] skip export: {src} missing")
        return
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    name = f"forgekey-{VERSION}-{COMMIT}.bin"
    dest = ARTIFACT_DIR / name
    shutil.copy2(src, dest)

    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    (ARTIFACT_DIR / f"{name}.sha256").write_text(digest + "\n")

    # Convenience copy for staff who just want "the latest". Not a symlink so
    # this works on Windows checkouts too.
    latest = ARTIFACT_DIR / "forgekey-latest.bin"
    shutil.copy2(dest, latest)
    (ARTIFACT_DIR / "forgekey-latest.bin.sha256").write_text(digest + "\n")

    size = dest.stat().st_size
    print(f"[version] exported {dest.relative_to(PROJECT_DIR)} ({size} bytes)")
    print(f"[version] sha256 {digest}")
    print(f"[version] upload this .bin to OMS; sha256 goes in the dispatch payload")


_inject_flags()

# Hook the post-build action onto firmware.bin (the merged binary that ends up
# on the device). PlatformIO emits this artifact under .pio/build/<env>/.
env.AddPostAction("$BUILD_DIR/firmware.bin", _export_artifact)  # noqa: F821
