#!/usr/bin/env python3
"""Construct real tmpfs roots through the production newc and VFS name walker."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Initramfs(unittest.TestCase):
    def test_root_objects_links_devices_and_rollback(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-initramfs-") as directory:
            work = Path(directory)
            (work / "usr").mkdir()
            (work / "usr/中文").write_bytes(b"native archive\n")
            os.link(work / "usr/中文", work / "hard")
            (work / "bin").symlink_to("usr")
            (work / "usr/中文").chmod(0o751)
            archive = work / "root.cpio"
            with archive.open("wb") as output:
                subprocess.run(["cpio", "-o", "-H", "newc", "--null", "--quiet"], cwd=work,
                    input="usr\0bin\0usr/中文\0hard\0".encode(), stdout=output,
                    stderr=subprocess.PIPE, check=True)
            binary = work / "unpack"
            subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/initramfs_test.c", "fs/initramfs/archive.c", "fs/initramfs/unpack.c",
                "fs/initramfs/view.c", "fs/initramfs/records.c", "fs/tmpfs/core.c",
                "fs/vfs/mount_tree.c", "fs/vfs/path.c", "fs/vfs/dentry.c",
                "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary), str(archive)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
