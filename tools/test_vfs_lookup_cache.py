#!/usr/bin/env python3
"""Exercise bounded name caching and provider coherency with production VFS."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LookupCache(unittest.TestCase):
    def test_lookup_cache(self):
        with tempfile.TemporaryDirectory(prefix="vfs-lookup-cache-", dir=os.environ["TMPDIR"]) as directory:
            binary = Path(directory) / "lookup-cache"
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/vfs_lookup_cache_test.c",
                "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c", "fs/vfs/path.c", "fs/vfs/lookup_cache.c",
                "-o", str(binary),
            ], cwd=ROOT, check=True)
            for case in ("positive", "negative", "generation", "parents", "dynamic", "stale_retain", "errors",
                         "permissions", "bounds", "oom", "budget"):
                with self.subTest(case=case):
                    subprocess.run([str(binary), case], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main(verbosity=2)
