#!/usr/bin/env python3
"""Run the installer RAM-root mapping regression against a real ext2 image."""
from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TEMP_PARENT = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
TEMP_PARENT.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="reliefos-installer-ramdisk-", dir=TEMP_PARENT) as directory:
    work = Path(directory)
    image = work / "installer-root.ext2"
    executable = work / "installer-ramdisk-test"
    subprocess.run([
        "mke2fs", "-q", "-t", "ext2", "-b", "1024", "-I", "256",
        "-O", "none", "-F", str(image), "8192",
    ], check=True)
    subprocess.run([
        "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
        "tools/tests/installer_ramdisk_test.c", "-o", str(executable),
    ], cwd=ROOT, check=True)
    subprocess.run([str(executable), str(image)], cwd=ROOT, check=True, timeout=30)
