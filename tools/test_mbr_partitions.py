#!/usr/bin/env python3
"""Check actual DOS primary/EBR parsing with bounded native disk fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class MBRPartitions(unittest.TestCase):
    def test_primary_logical_and_corruption(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-mbr-") as directory:
            binary=Path(directory)/"mbr"
            subprocess.run(["clang","-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-fno-sanitize-recover=all","-Iinclude","-Iinclude/uapi",
                "tools/tests/mbr_partitions_test.c","block/partition/mbr.c","-o",str(binary)],cwd=ROOT,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)


if __name__=="__main__":unittest.main()
