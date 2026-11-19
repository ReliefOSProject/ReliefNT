#!/usr/bin/env python3
"""Run real FAT directory/cache reads through native split and retry ownership."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--block-source", type=Path, default=ROOT / "block/core/storage_block.c")
parser.add_argument("--expect-stuck", action="store_true", help="Characterize the original MDTS=1 livelock")
args = parser.parse_args()
temporary = Path(os.environ.get("TMPDIR", ROOT / "out/tmp")).resolve()
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="fat-async-read-", dir=temporary) as directory:
    binary = Path(directory) / "fat-async-read"
    subprocess.run([
        "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Iinclude", "-Iinclude/uapi",
        "-I" + str(ROOT / "block/core"), "tools/tests/fat_read_split_test.c",
        "block/core/storage_state.c", "block/core/device_number.c", "block/core/storage_state_data.c",
        "block/device/registry.c", str(args.block_source.resolve()),
        "fs/fat/fat32_io.c", "fs/fat/fat32_name.c", "fs/fat/fat32_dir.c", "lib/text_utf16.c",
        "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary), *(["--expect-stuck"] if args.expect_stuck else [])],
                   cwd=ROOT, check=True, timeout=120)
