#!/usr/bin/env python3
"""Check module syscall boundaries without invoking the host kernel module syscalls."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class ModuleSyscalls(unittest.TestCase):
    def test_permissions_copies_file_and_errors(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-syscalls-") as directory:
            binary = Path(directory) / "test"
            for definitions in (("-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1"),
                                ("-DCONFIG_MODULES=1",), ()):
                subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", *definitions,
                            "-Iinclude", "-Iinclude/uapi",
                            "tools/tests/module_syscalls_test.c", "kernel/syscall/system/modules.c",
                            "kernel/module/file.c", "fs/vfs/write_access.c", "fs/vfs/mount_tree.c", "fs/vfs/path.c",
                            "fs/vfs/dentry.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c", "-o", str(binary)], cwd=ROOT, check=True)
                subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
