#!/usr/bin/env python3
"""Exercise real PIO task-file bytes and CPU transfers at an emulated port boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class IdePio(unittest.TestCase):
    def test_standard_taskfile_and_arbitrary_cpu_alignment(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-ide-pio-") as directory:
            binary = Path(directory) / "pio"
            result = subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/ide_pio_test.c", "drivers/storage/ide/storage_ide.c", "-o", str(binary)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
