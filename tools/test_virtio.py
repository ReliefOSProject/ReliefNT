#!/usr/bin/env python3
"""Run actual split-ring code with a deterministic device-side ring producer."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VirtioQueue(unittest.TestCase):
    def test_split_ring(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-virtio-") as directory:
            binary = Path(directory) / "queue"
            subprocess.run([
                os.environ.get("HOSTCC", "clang"), "-std=gnu11", "-O1", "-g",
                "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
                "-pthread",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/virtio_queue_test.c",
                "drivers/virtio/queue.c", "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
