#!/usr/bin/env python3
"""Check real shared UC/NX mappings for PCI BARs above the physical direct map."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class IoRemap(unittest.TestCase):
    def test_shared_mapping_and_rollback(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-ioremap-") as directory:
            for modules in (False, True):
                binary = Path(directory) / ("ioremap-modules" if modules else "ioremap-builtin")
                result = subprocess.run([
                    "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    *(["-DCONFIG_MODULES=1"] if modules else []),
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/ioremap_test.c",
                    "arch/x86_64/mm/ioremap.c", "-o", str(binary),
                ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode, 0, result.stdout)
                subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
