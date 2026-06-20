#!/usr/bin/env python3
"""Validate the ePaper bind QR payload, capacity, and panel layout."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import uuid
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
EPAPER_SRC = ROOT / "src" / "capabilities" / "epaper_pm" / "epaper_pm_capability.cpp"
DEVICE_CONFIG = ROOT / "src" / "provisioning" / "device_config.h"

RAW_DATA_MODULES = [
    208, 359, 567, 807, 1079, 1383, 1568, 1936, 2336, 2768,
    3232, 3728, 4256, 4651, 5243, 5867, 6523, 7211, 7931, 8683,
    9252, 10068, 10916, 11796, 12708, 13652, 14628, 15371, 16411,
    17483, 18587, 19723, 20891, 22091, 23008, 24272, 25568, 26896,
    28256, 29648,
]

ECC_CODEWORDS = {
    "ECC_MEDIUM": [
        10, 16, 26, 36, 48, 64, 72, 88, 110, 130, 150, 176, 198, 216,
        240, 280, 308, 338, 364, 416, 442, 476, 504, 560, 588, 644,
        700, 728, 784, 812, 868, 924, 980, 1036, 1064, 1120, 1204,
        1260, 1316, 1372,
    ],
    "ECC_LOW": [
        7, 10, 15, 20, 26, 36, 40, 48, 60, 72, 80, 96, 104, 120,
        132, 144, 168, 180, 196, 224, 224, 252, 270, 300, 312, 336,
        360, 390, 420, 450, 480, 510, 540, 570, 570, 600, 630, 660,
        720, 750,
    ],
    "ECC_HIGH": [
        17, 28, 44, 64, 88, 112, 130, 156, 192, 224, 264, 308, 352,
        384, 432, 480, 532, 588, 650, 700, 750, 816, 900, 960, 1050,
        1110, 1200, 1260, 1350, 1440, 1530, 1620, 1710, 1800, 1890,
        1980, 2100, 2220, 2310, 2430,
    ],
    "ECC_QUARTILE": [
        13, 22, 36, 52, 72, 96, 108, 132, 160, 192, 224, 260, 288,
        320, 360, 408, 448, 504, 546, 600, 644, 690, 750, 810, 870,
        952, 1020, 1050, 1140, 1200, 1290, 1350, 1440, 1530, 1590,
        1680, 1770, 1860, 1950, 2040,
    ],
}

ECC_NUMERIC = {
    "ECC_LOW": 0,
    "ECC_MEDIUM": 1,
    "ECC_QUARTILE": 2,
    "ECC_HIGH": 3,
}


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def read_text(path: Path) -> str:
    try:
        return path.read_text()
    except FileNotFoundError:
        fail(f"missing {path}")


def c_string_macro(text: str, name: str) -> str:
    match = re.search(rf'#define\s+{re.escape(name)}\s+"([^"]*)"', text)
    if not match:
        fail(f"missing string macro {name}")
    return match.group(1)


def c_int_macro(text: str, name: str) -> int:
    match = re.search(rf"#define\s+{re.escape(name)}\s+([0-9]+)", text)
    if not match:
        fail(f"missing integer macro {name}")
    return int(match.group(1))


def source_int(text: str, name: str) -> int:
    match = re.search(
        rf"constexpr\s+(?:uint8_t|size_t|int)\s+{re.escape(name)}\s*=\s*([0-9]+)\s*;",
        text,
    )
    if not match:
        fail(f"missing integer constexpr {name}")
    return int(match.group(1))


def source_ecc(text: str) -> str:
    match = re.search(r"constexpr\s+uint8_t\s+kQrEcc\s*=\s*(ECC_[A-Z]+)\s*;", text)
    if not match:
        fail("missing kQrEcc")
    ecc = match.group(1)
    if ecc not in ECC_CODEWORDS:
        fail(f"unknown kQrEcc {ecc}")
    return ecc


def qr_byte_capacity(version: int, ecc: str) -> int:
    if version < 1 or version > 40:
        fail(f"unsupported QR version {version}")
    data_codewords = RAW_DATA_MODULES[version - 1] // 8 - ECC_CODEWORDS[ecc][version - 1]
    char_count_bits = 8 if version <= 9 else 16
    return (data_codewords * 8 - 4 - char_count_bits) // 8


def bind_url(host: str, port: int, display_id: str) -> str:
    scheme = "https" if port == 443 else "http"
    authority = host if port in (80, 443) else f"{host}:{port}"
    return f"{scheme}://{authority}/forgekey/epaper/bind?did={display_id}"


def validate_url(url: str, expected_display_id: str) -> None:
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https"):
        fail(f"unexpected URL scheme: {parsed.scheme}")
    if not parsed.netloc:
        fail("URL is missing host")
    if parsed.path != "/forgekey/epaper/bind":
        fail(f"unexpected bind path: {parsed.path}")
    did_values = parse_qs(parsed.query).get("did", [])
    if did_values != [expected_display_id]:
        fail(f"unexpected did query value: {did_values}")
    try:
        parsed_uuid = uuid.UUID(expected_display_id)
    except ValueError as exc:
        fail(f"display_id is not a UUID: {exc}")
    if parsed_uuid.version != 4:
        fail(f"display_id must be UUIDv4, got v{parsed_uuid.version}")


def validate_geometry(version: int, module_px: int, quiet_modules: int, origin_y: int, footer_gap: int) -> None:
    panel_width = source_int(read_text(EPAPER_SRC), "kPanelWidth")
    panel_height = source_int(read_text(EPAPER_SRC), "kPanelHeight")
    qr_size = 4 * version + 17
    qr_pixels = qr_size * module_px
    quiet_px = quiet_modules * module_px
    origin_x = (panel_width - qr_pixels) // 2
    footer_y = origin_y + qr_pixels + footer_gap

    if module_px < 4:
        fail(f"module size too small for phone scanning: {module_px}px")
    if quiet_modules < 4:
        fail(f"quiet zone too small: {quiet_modules} modules")
    if origin_x < quiet_px:
        fail("left quiet zone falls off panel")
    if origin_y < quiet_px:
        fail("top quiet zone falls off panel")
    if origin_x + qr_pixels + quiet_px > panel_width:
        fail("right quiet zone falls off panel")
    if origin_y + qr_pixels + quiet_px > panel_height:
        fail("bottom quiet zone falls off panel")
    if footer_y + 76 > panel_height:
        fail("footer text does not fit below QR")


def run_library_smoke(url: str, version: int, ecc: str) -> str:
    qrcode_dir = ROOT / ".pio" / "libdeps" / "seeed_xiao_epaper" / "QRCode" / "src"
    qrcode_c = qrcode_dir / "qrcode.c"
    qrcode_h = qrcode_dir / "qrcode.h"
    cc = shutil.which("cc")
    if not cc or not qrcode_c.exists() or not qrcode_h.exists():
        return "qr_library=skipped"

    program = textwrap.dedent(
        """
        #include <stdio.h>
        #include <string.h>
        #include "qrcode.h"

        #ifndef QR_VERSION
        #error QR_VERSION missing
        #endif
        #ifndef QR_ECC
        #error QR_ECC missing
        #endif

        int main(int argc, char **argv) {
            if (argc != 2) return 10;
            QRCode qr;
            unsigned char modules[qrcode_getBufferSize(QR_VERSION)];
            int rc = qrcode_initText(&qr, modules, QR_VERSION, QR_ECC, argv[1]);
            if (rc != 0) return 20;
            if (qr.size != (QR_VERSION * 4 + 17)) return 30;
            unsigned dark = 0;
            for (unsigned y = 0; y < qr.size; y++) {
                for (unsigned x = 0; x < qr.size; x++) {
                    if (qrcode_getModule(&qr, x, y)) dark++;
                }
            }
            if (dark == 0) return 40;
            printf("qr_library=ok size=%u mode=%u mask=%u dark=%u", qr.size, qr.mode, qr.mask, dark);
            return 0;
        }
        """
    )

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        c_path = tmp_path / "validate_qr.c"
        exe_path = tmp_path / "validate_qr"
        c_path.write_text(program)
        build = subprocess.run(
            [
                cc,
                "-I",
                str(qrcode_dir),
                f"-DQR_VERSION={version}",
                f"-DQR_ECC={ECC_NUMERIC[ecc]}",
                str(c_path),
                str(qrcode_c),
                "-o",
                str(exe_path),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if build.returncode != 0:
            return "qr_library=compile_failed"
        run = subprocess.run(
            [str(exe_path), url],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if run.returncode != 0:
            fail(f"QRCode library smoke test failed rc={run.returncode}: {run.stderr.strip()}")
        return run.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("OMS_HOST"))
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument(
        "--display-id",
        default="00000000-0000-4000-8000-000000000000",
        help="sample UUIDv4 display_id to validate",
    )
    args = parser.parse_args()

    config_text = read_text(DEVICE_CONFIG)
    source_text = read_text(EPAPER_SRC)

    host = args.host or c_string_macro(config_text, "OMS_HOST")
    port = args.port if args.port is not None else int(os.environ.get("OMS_PORT") or c_int_macro(config_text, "OMS_PORT"))
    url = bind_url(host, port, args.display_id)
    payload_len = len(url.encode("utf-8"))

    version = source_int(source_text, "kQrVersion")
    module_px = source_int(source_text, "kQrModulePx")
    quiet_modules = source_int(source_text, "kQrQuietModules")
    max_payload = source_int(source_text, "kQrMaxPayloadBytes")
    origin_y = source_int(source_text, "kQrOriginY")
    footer_gap = source_int(source_text, "kQrFooterGapPx")
    ecc = source_ecc(source_text)
    spec_capacity = qr_byte_capacity(version, ecc)

    validate_url(url, args.display_id)
    if max_payload != spec_capacity:
        fail(f"kQrMaxPayloadBytes={max_payload}, but spec capacity is {spec_capacity}")
    if payload_len > spec_capacity:
        fail(f"payload is {payload_len} bytes, QR capacity is {spec_capacity}")
    validate_geometry(version, module_px, quiet_modules, origin_y, footer_gap)
    smoke = run_library_smoke(url, version, ecc)

    print("PASS epaper QR validation")
    print(f"url={url}")
    print(f"payload_bytes={payload_len} capacity_bytes={spec_capacity}")
    print(f"version={version} ecc={ecc} module_px={module_px} quiet_modules={quiet_modules}")
    print(smoke)
    return 0


if __name__ == "__main__":
    sys.exit(main())
