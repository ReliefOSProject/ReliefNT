#!/usr/bin/env python3
"""Exercise real EHCI descriptors/configuration and shared USB boot HID."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class USBEnhancedHost(unittest.TestCase):
    def test_dma_configuration_and_input(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-ehci-") as directory:
            binary = Path(directory) / "ehci"
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/usb_ehci_test.c", "drivers/usb/ehci/transfer.c",
                "drivers/usb/core/descriptors.c", "drivers/usb/ehci/clock.c", "drivers/usb/core/hid.c",
                "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


    def test_periodic_admission_and_tree(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-ehci-bandwidth-") as directory:
            binary = Path(directory) / "ehci-bandwidth"
            sources = ["tools/tests/usb_ehci_bandwidth_test.c", "drivers/usb/ehci/periodic.c",
                       "drivers/usb/ehci/transfer.c", "drivers/usb/ehci/bandwidth.c"]
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                *sources, "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
