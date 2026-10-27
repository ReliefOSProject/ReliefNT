#!/usr/bin/env python3
"""Exercise owned coherent DMA buffers with a disjoint CPU/physical address map."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DmaBuffer(unittest.TestCase):
    def test_owned_backing_and_quiescence(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-dma-") as directory:
            binary = Path(directory) / "dma-buffer"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/dma_buffer_test.c", "kernel/dma/buffer.c",
                "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
