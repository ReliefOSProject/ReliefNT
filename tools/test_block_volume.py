#!/usr/bin/env python3
"""Exercise real volume-to-block ownership and transport-independent I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class BlockVolume(unittest.TestCase):
    def test_owned_transport(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-block-volume-") as directory:
            binary = Path(directory) / "block-volume"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-DCONFIG_MODULES=1", "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/block_volume_test.c", "block/device/registry.c",
                "block/core/volume.c", "block/core/storage_block.c", "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
