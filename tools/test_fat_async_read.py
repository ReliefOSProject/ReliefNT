#!/usr/bin/env python3
"""Bound asynchronous FAT retry progress using production reader/cache code."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="fat-async-read-", dir=os.environ.get("TMPDIR")) as temporary:
        executable = Path(temporary) / "fat-async-read"
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-misleading-indentation",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "--param=asan-globals=0",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Iinclude", "-Iinclude/uapi",
            "tools/tests/fat_async_read_test.c", "fs/fat/fat32_io.c", "fs/fat/fat32_dir.c",
            "fs/fat/fat32_name.c", "lib/text_utf16.c", "-o", str(executable),
        ], cwd=ROOT, check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
