#!/usr/bin/env python3
"""Check bounded native ext data batching, allocation fallback and partial errors."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="ext4-write-batch-") as temporary:
    binary = Path(temporary) / "batch"
    subprocess.run([
        "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Iinclude",
        "-Iinclude/uapi", "tools/tests/ext4_write_batch_test.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary), *sys.argv[1:]], cwd=ROOT, check=True, timeout=30)
    if "--baseline" not in sys.argv[1:]:
        range_writer = Path(temporary) / "range-writer"
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Iinclude",
            "-Iinclude/uapi", "tools/tests/ext4_write_range_alloc_test.c",
            "fs/ext_common/ops.c", "fs/ext_common/format.c", "fs/ext_common/checksum.c",
            "-o", str(range_writer),
        ], cwd=ROOT, check=True)
        subprocess.run([str(range_writer)], cwd=ROOT, check=True, timeout=30)
        transport = Path(temporary) / "transport"
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
            "tools/tests/ext4_write_batch_transport_test.c", "-o", str(transport),
        ], cwd=ROOT, check=True)
        subprocess.run([str(transport)], cwd=ROOT, check=True, timeout=30)

print("PASS ext data write staging and device submission counts")
