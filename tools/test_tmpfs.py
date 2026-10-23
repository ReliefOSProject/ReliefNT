#!/usr/bin/env python3
"""Exercise the real tmpfs backend with ASan/UBSan and bounded RAM fixtures."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-tmpfs-") as directory:
    out = pathlib.Path(directory)
    subprocess.run(["clang", "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                    "tools/tests/tmpfs_test.c", "fs/tmpfs/core.c", "-o", str(out / "tmpfs-test")],
                   cwd=ROOT, check=True)
    subprocess.run([str(out / "tmpfs-test")], cwd=ROOT, check=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-DTMPFS_MMAP_STANDALONE",
                    "tools/tests/tmpfs_mmap_test.c", "-o", str(out / "mmap-reference")],
                   cwd=ROOT, check=True)
    subprocess.run([str(out / "mmap-reference")], cwd=ROOT, check=True, timeout=30)
