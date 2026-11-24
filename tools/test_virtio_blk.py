#!/usr/bin/env python3
"""Check VirtIO-Block requests, geometry and ownership with the actual split ring."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VirtioBlock(unittest.TestCase):
    def test_device_protocol(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-virtio-blk-") as directory:
            binary = Path(directory) / "virtio-blk"
            subprocess.run([
                os.environ.get("HOSTCC", "clang"), "-std=gnu11", "-O1", "-g",
                "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/virtio_blk_test.c",
                "drivers/storage/virtio_blk/request.c", "drivers/virtio/queue.c",
                "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
