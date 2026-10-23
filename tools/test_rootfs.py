#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RootfsInterfaces(unittest.TestCase):
    def test_kernel_modules(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-rootfs-") as directory:
            for name in ("rootfs_mounts", "rootfs_uts", "rootfs_paths", "procfs_directories", "linux_permissions", "chroot_paths"):
                with self.subTest(module=name):
                    executable = Path(directory) / name
                    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                                    "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                                    f"tools/tests/{name}_test.c", "-o", executable], cwd=ROOT, check=True)
                    subprocess.run([executable], cwd=ROOT, check=True, timeout=20)



if __name__ == "__main__":
    unittest.main()
