#!/usr/bin/env python3
"""Run Multiboot installer-root and SquashFS payload registration checks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-squashfs-boot-") as directory:
    executable = Path(directory) / "squashfs-boot-modules-test"
    subprocess.run([
        *shlex.split(os.environ.get("HOSTCC", "cc")), "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-error=misleading-indentation",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/squashfs_boot_modules_test.c",
        "block/ramdisk/boot.c", "-o", str(executable),
    ], cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True, timeout=10)
