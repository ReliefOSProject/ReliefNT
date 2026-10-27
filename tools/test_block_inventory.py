#!/usr/bin/env python3
"""Exercise real registry/inventory identity across repeated native transport reloads."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class Inventory(unittest.TestCase):
    def test_names_stale_ids_and_growth(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-block-inventory-") as directory:
            binary = Path(directory) / "inventory"
            result = subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi", "tools/tests/block_inventory_test.c",
                "block/device/registry.c", "block/device/inventory.c", "block/core/device_number.c",
                "block/core/storage_state_data.c", "block/partition/partition_install.c", "-o", str(binary)], cwd=ROOT,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
