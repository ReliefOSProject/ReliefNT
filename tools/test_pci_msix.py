#!/usr/bin/env python3
"""Verify production MSI-X allocation and IRQ lifetime at the PCI boundary."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="reliefnt-msix-", dir=temporary) as work:
    binary = Path(work) / "msix"
    subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/pci_msix_test.c",
                    "arch/x86_64/platform/pci_irq.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
