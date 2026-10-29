#!/usr/bin/env python3
"""Check actual UHCI DMA translation and quiescence at its hardware boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class USBDMA(unittest.TestCase):
    def test_owned_backing_and_stop(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-usb-dma-") as directory:
            binary = Path(directory) / "usb-dma"
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-DUHCI_TESTING=1",
                "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/usb_dma_test.c", "drivers/usb/core/hid.c", "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
