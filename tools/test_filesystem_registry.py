#!/usr/bin/env python3
"""Exercise filesystem admission with the actual VFS and module lifecycle."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FilesystemRegistry(unittest.TestCase):
    def test_admission_mount_pins_and_rollback(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-filesystems-") as directory:
            binary = Path(directory) / "filesystems"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/filesystem_registry_test.c",
                "fs/vfs/filesystems.c", "fs/vfs/filesystem_instance.c", "fs/vfs/mount_tree.c", "kernel/module/lifecycle.c", "kernel/params/value.c",
                "kernel/module/layout.c", "kernel/module/symbols.c", "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
