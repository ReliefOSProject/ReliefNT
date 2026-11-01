#!/usr/bin/env python3
"""Link EFI ACPI discovery to production topology and bootstrap admission."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-efi-acpi-bootstrap-") as work:
    binary = str(Path(work) / "test")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/efi_acpi_bootstrap_test.c", "mm/physical/topology.c",
                    "mm/physical/bootstrap.c", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True, timeout=20)
