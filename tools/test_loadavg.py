#!/usr/bin/env python3
"""Run the scheduler load-average update math with known load histories."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="loadavg-") as temporary:
    binary = Path(temporary) / "loadavg"
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Iinclude",
                    "tools/tests/loadavg_test.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True)
