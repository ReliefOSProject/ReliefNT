#!/usr/bin/env python3
"""Check ext-family mapping reuse and allocation searches against bit/byte oracles."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="ext4-io-search-") as temporary:
    for name, sources in (
        ("bitmap", ["tools/tests/ext4_bitmap_scan_test.c", "fs/ext_common/format.c"]),
        ("mapping", ["tools/tests/ext4_read_mapping_test.c", "fs/ext_common/ops.c"]),
        ("resume", ["tools/tests/ext4_read_batch_resume_test.c"]),
    ):
        binary = Path(temporary) / name
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Iinclude",
            "-Iinclude/uapi", *sources, "-o", str(binary),
        ], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=120)

print("PASS ext-family bounded mapping and bitmap searches")
