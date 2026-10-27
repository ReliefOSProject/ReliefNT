#!/usr/bin/env python3
"""Validate native newc decoder boundaries and independent GNU cpio archives."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class InitramfsArchive(unittest.TestCase):
    def test_wire_bounds_checksums_and_gnu_cpio(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-newc-") as directory:
            work = Path(directory)
            (work / "dir").mkdir()
            (work / "sample").write_bytes(b"hello\n")
            (work / "sample").chmod(0o640)
            os.link(work / "sample", work / "hard")
            (work / "alias").symlink_to("sample")
            (work / "dir/中文").write_bytes(b"utf8")
            archive = work / "reference.cpio"
            with archive.open("wb") as output:
                result = subprocess.run(["cpio", "-o", "-H", "newc", "--null", "--quiet"], cwd=work,
                    input="dir\0sample\0hard\0alias\0dir/中文\0".encode(), stdout=output, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            binary = work / "newc"
            result = subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/initramfs_archive_test.c", "fs/initramfs/archive.c", "-o", str(binary)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary), str(archive)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
