#!/usr/bin/env python3
"""Exercise USB host publication/drain against the actual native module lifetime."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ModuleUSBHost(unittest.TestCase):
    def test_publication_fair_polling_and_unload(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-usb-owner-") as directory:
            binary = Path(directory) / "usb-owner"
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/module_usb_owner_test.c",
                "drivers/usb/core/registry.c", "kernel/module/lifecycle.c", "kernel/params/value.c",
                "kernel/module/layout.c", "kernel/module/symbols.c", "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
