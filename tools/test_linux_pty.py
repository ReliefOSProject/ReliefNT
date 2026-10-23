#!/usr/bin/env python3
"""Exercise the real kernel PTY implementation with native Linux encodings."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-linux_pty-") as directory:
    binary = Path(directory) / "linux-pty-test"
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-Iinclude", "-Iinclude", "-Iinclude/uapi",
        "-Iinclude", "-Wl,--gc-sections",
        "tools/tests/linux_pty_test.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True)
