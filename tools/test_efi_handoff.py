#!/usr/bin/env python3
"""Exercise production firmware handoff and its boot coordinator on the host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-efi-handoff-") as work:
    binary = Path(work) / "efi_handoff"
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/efi_handoff_test.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
