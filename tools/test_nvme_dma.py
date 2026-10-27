#!/usr/bin/env python3
"""Exercise native NVMe full-width PRPs and failed controller STOP ownership."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class NvmeDma(unittest.TestCase):
    def test_stop_and_prp(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-nvme-dma-") as directory:
            binary = Path(directory) / "nvme-dma"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/nvme_dma_test.c", "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
