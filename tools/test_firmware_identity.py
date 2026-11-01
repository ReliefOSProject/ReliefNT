#!/usr/bin/env python3
"""Exercise EFI identity acquisition against real production SMBIOS parsing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-firmware-identity-") as work:
    binary = str(Path(work) / "identity")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/firmware_identity_test.c", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True, timeout=20)
