#!/usr/bin/env python3
"""Exercise production ACPI lookup through identity and direct-map aliases."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-acpi-direct-") as work:
    binary = Path(work) / "acpi-direct"
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/acpi_directmap_test.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
