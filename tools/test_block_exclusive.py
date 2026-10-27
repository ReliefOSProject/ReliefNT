#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BlockExclusiveTests(unittest.TestCase):
    def test_exclusive_block_open_distinguishes_optical_and_mounted_disks(self):
        """Keep mounted partitions protected while allowing a separate blank disk."""
        with tempfile.TemporaryDirectory(prefix="block-exclusive-") as tmp:
            executable = str(Path(tmp) / "exclusive")
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Wno-address-of-packed-member", "-Iinclude",
                "-Iinclude", "-Iinclude/uapi",
                "tools/tests/block_exclusive_mount_test.c", "fs/tmpfs/core.c",
                "lib/text_utf16.c", "fs/vfs/filesystems.c", "fs/vfs/filesystem_instance.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=20)



if __name__ == "__main__":
    unittest.main()
