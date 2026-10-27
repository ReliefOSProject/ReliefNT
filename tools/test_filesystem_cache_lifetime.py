#!/usr/bin/env python3
"""Prove native ext cache backing is reclaimed only after dirty/pinned users drain."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class FilesystemCaches(unittest.TestCase):
    def test_private_pages_and_exit(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-fs-cache-") as directory:
            binary = Path(directory) / "fs-cache"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/filesystem_cache_lifetime_test.c",
                "block/core/storage_state.c", "block/core/storage_state_data.c", "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
