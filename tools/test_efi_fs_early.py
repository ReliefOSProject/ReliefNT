#!/usr/bin/env python3
"""Build and run the EFI filesystem's pre-page-table lifecycle fixture."""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="reliefos-efi-fs-", dir=temporary) as directory:
    output = str(Path(directory) / "efi_fs_early")
    subprocess.run([
        "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-fno-pie", "-no-pie",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/efi_fs_early_test.c",
        "-o", output,
    ], cwd=ROOT, check=True)
    subprocess.run([output], cwd=ROOT, check=True, timeout=20)
