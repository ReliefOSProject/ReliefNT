#!/usr/bin/env python3
"""Validate Multiboot archive classification and distinct init/rdinit selection."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-initramfs-boot-") as directory:
    binary = Path(directory) / "boot"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/initramfs_boot_test.c", "fs/initramfs/source.c", "kernel/init/init_program.c",
        "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
