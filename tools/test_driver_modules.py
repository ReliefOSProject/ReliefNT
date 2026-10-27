#!/usr/bin/env python3
"""Exercise native driver bindings against the actual manager and callback gates."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class DriverModules(unittest.TestCase):
    def test_owner_cleanup_admission_and_rollback(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-driver-modules-") as directory:
            binary = Path(directory) / "bindings"
            subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                            "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1", "-pthread",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/driver_modules_test.c",
                            "kernel/device/registry.c", "kernel/device/services.c",
                            "kernel/device/driver_manager_phase.c", "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
