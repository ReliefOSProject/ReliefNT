#!/usr/bin/env python3
"""Check production paging coverage against hosted page-table fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-memory-layout-") as work:
    binary = Path(work) / "memory_layout"
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/memory_layout_test.c", "arch/x86_64/mm/direct_map.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
