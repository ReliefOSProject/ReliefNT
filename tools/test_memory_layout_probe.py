#!/usr/bin/env python3
"""Exercise the external high-RAM probe against owned host-backed pages."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-memory-probe-") as work:
    binary = Path(work) / "probe-test"
    subprocess.run([
        "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/memory_layout_probe_test.c",
        "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
