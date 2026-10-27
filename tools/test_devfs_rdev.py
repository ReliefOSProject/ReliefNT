#!/usr/bin/env python3
"""Resolve generated device identities independently of their namespace paths."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-devfs-rdev-") as directory:
    binary = Path(directory) / "devices"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/devfs_rdev_test.c", "fs/devfs/device.c", "fs/devfs/identity.c", "-o", str(binary)],
        cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
