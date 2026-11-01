#!/usr/bin/env python3
"""Check the native SVGA binding against an allocator with high and low RAM."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-svga-ppn-") as work:
    binary = Path(work) / "svga-ppn"
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/svga_ppn_alloc_test.c", "drivers/video/svga/gb.c",
        "drivers/video/svga/gmr.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
