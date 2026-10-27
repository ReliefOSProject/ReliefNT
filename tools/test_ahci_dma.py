#!/usr/bin/env python3
"""Check the actual AHCI DMA descriptors and owned per-port physical backing."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class AhciDma(unittest.TestCase):
    def test_descriptors_and_retirement(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-ahci-dma-") as directory:
            binary = Path(directory) / "ahci-dma"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/ahci_dma_test.c", "kernel/dma/buffer.c",
                "drivers/storage/ahci/dma.c", "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
