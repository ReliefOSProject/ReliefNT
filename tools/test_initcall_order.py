#!/usr/bin/env python3
"""Run the actual linked early filesystem and ordinary builtin initcall phases."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InitcallOrder(unittest.TestCase):
    def test_filesystems_before_devices(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-initcalls-") as directory:
            work = Path(directory)
            script = work / "initcalls.ld"
            script.write_text('''SECTIONS {
 .reliefnt_initcalls : {
  __start_reliefnt_fs_initcalls = .;
  KEEP(*(.reliefnt.fs_initcall))
  __stop_reliefnt_fs_initcalls = .;
  __start_reliefnt_block_initcalls = .;
  KEEP(*(.reliefnt.block_initcall))
  __stop_reliefnt_block_initcalls = .;
  __start_reliefnt_subsys_initcalls = .;
  KEEP(*(.reliefnt.subsys_initcall))
  __stop_reliefnt_subsys_initcalls = .;
  __start_reliefnt_initcalls = .;
  KEEP(*(.reliefnt.initcall))
  __stop_reliefnt_initcalls = .;
 }
} INSERT AFTER .rodata;
''')
            binary = work / "initcalls"
            result = subprocess.run([
                "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/initcall_order_test.c", "kernel/init/initcalls.c",
                "-Wl,-T," + str(script), "-o", str(binary),
            ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
