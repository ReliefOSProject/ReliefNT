#!/usr/bin/env python3
"""Exercise referenced mounts/paths against real native module ownership."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class MountTree(unittest.TestCase):
    def test_namespace_paths_and_owner_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-mount-tree-") as directory:
            binary = Path(directory) / "mount-tree"
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/mount_tree_test.c",
                "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c", "fs/vfs/path.c", "fs/vfs/dentry.c", "fs/vfs/file.c", "fs/vfs/open.c",
                "fs/vfs/pseudo.c", "fs/vfs/attributes.c", "kernel/module/lifecycle.c", "kernel/params/value.c",
                "kernel/module/layout.c", "kernel/module/symbols.c", "-o", str(binary)
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

if __name__ == "__main__":
    unittest.main()
