#!/usr/bin/env python3
"""Validate actual exFAT OEM boot parsing against independent wire/checksum data."""
from pathlib import Path
from storage_graph_test_sources import STORAGE_GRAPH_SOURCES
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-exfat-boot-") as directory:
    binary = Path(directory) / "boot"
    subprocess.run([
        "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/exfat_boot_test.c",
        "fs/exfat/exfat_cache.c", "block/core/storage_state.c",
        "block/core/storage_state_data.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
    mount = Path(directory) / "mount"
    subprocess.run([
        "clang", "-std=c11", "-DRELIEFNT_FIXTURE_REAL_DAC", "-O1", "-g",
        "-fsanitize=address,undefined", "-mllvm", "-asan-globals=0", "-fno-sanitize-recover=all",
        "-no-pie", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/exfat_mount_reserved_test.c",
        "fs/tmpfs/core.c", "mm/physical/proc.c", "fs/vfs/exec_file.c", "lib/text_utf16.c",
        *STORAGE_GRAPH_SOURCES, "-o", str(mount),
    ], cwd=ROOT, check=True)
    image = Path(directory) / "formatter.img"
    with image.open("wb") as stream:
        stream.truncate(128 * 1024 * 1024)
    subprocess.run(["mkfs.exfat", str(image)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(mount), str(image)], cwd=ROOT,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"), check=True, timeout=45)
