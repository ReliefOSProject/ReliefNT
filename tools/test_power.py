#!/usr/bin/env python3
"""Test the real reboot syscall without executing host power operations."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefos-power-") as directory:
    output = str(Path(directory) / "reboot")
    # Specialize the included dispatcher for SYS_reboot, discarding unrelated
    # kernel handlers. Only the final hardware operations are intercepted.
    subprocess.run([
        "cc", "-std=c11", "-O2", "-flto", "-fwhole-program",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
        "tools/tests/reboot_syscall_test.c", "-o", output,
    ], cwd=ROOT, check=True)
    subprocess.run([output], cwd=ROOT, check=True, timeout=10)
