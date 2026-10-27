#!/usr/bin/env python3
"""Validate real registered native formatters with independent fsck tools."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FilesystemFormat(unittest.TestCase):
    def test_native_bytes_bounds_and_provider_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-fs-format-") as directory:
            work = Path(directory)
            binary = work / "format"
            native_formats = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "fs").glob("*/mkfs.c"))
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1", "-fno-pie", "-no-pie",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/filesystem_format_test.c",
                "block/install/storage_installer.c", "block/core/storage_state.c", "block/core/storage_state_data.c",
                "fs/vfs/filesystems.c", "fs/exfat/exfat_name.c", "lib/text_utf16.c", *native_formats,
                "kernel/module/lifecycle.c", "kernel/params/value.c", "kernel/module/layout.c", "kernel/module/symbols.c", "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            for name, checker in (("vfat", ["fsck.fat", "-n"]), ("exfat", ["fsck.exfat", "-n"]),
                                  ("ext2", ["e2fsck", "-f", "-n"]), ("ext4", ["e2fsck", "-f", "-n"])):
                image = work / (name + ".img")
                subprocess.run([str(binary), name, str(image)], check=True, timeout=30)
                subprocess.run([*checker, str(image)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
