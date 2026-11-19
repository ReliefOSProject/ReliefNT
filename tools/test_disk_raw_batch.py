#!/usr/bin/env python3
"""Check real raw-disk staging against a sector-accurate registered provider."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-disk-raw-batch-") as directory:
    binary = Path(directory) / "raw"
    subprocess.run([
        "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/disk_raw_batch_test.c",
        "block/partition/disk_io.c", "block/device/registry.c",
        "block/core/storage_state.c", "block/core/storage_state_data.c",
        "block/core/storage_block.c", "block/core/volume.c", "block/install/storage_installer.c",
        "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
