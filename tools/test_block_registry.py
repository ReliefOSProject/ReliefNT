#!/usr/bin/env python3
"""Exercise actual block admission/I/O and native module cleanup/refusal paths."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class BlockRegistry(unittest.TestCase):
    def test_module_and_device_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-block-registry-") as directory:
            binary = Path(directory) / "block-registry"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1", "-fno-pie", "-no-pie",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/block_registry_test.c",
                "block/device/registry.c", "kernel/module/lifecycle.c", "kernel/params/value.c",
                "kernel/module/layout.c", "kernel/module/symbols.c", "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=15)


    def test_physical_disconnect_and_publication_race(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-block-disconnect-") as directory:
            binary=Path(directory)/"disconnect"
            subprocess.run(["clang","-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
                "-DCONFIG_MODULES=1","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
                "-fno-pie","-no-pie","-fsanitize=address,undefined","-fno-sanitize-recover=all",
                "-Iinclude","-Iinclude/uapi","tools/tests/block_disconnect_test.c",
                "block/device/registry.c","-o",str(binary)],cwd=ROOT,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)


if __name__ == "__main__":
    unittest.main()
