#!/usr/bin/env python3
"""Run real native builtin init records, including a failed predecessor."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Initcalls(unittest.TestCase):
    def test_run_all_once_and_report_first_failure(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-initcalls-") as directory:
            work = Path(directory)
            script = work / "initcalls.lds"
            script.write_text("SECTIONS { .reliefnt.initcalls : ALIGN(8) { "
                              "__start_reliefnt_fs_initcalls = .; KEEP(*(.reliefnt.fs_initcall)) "
                              "__stop_reliefnt_fs_initcalls = .; "
                              "__start_reliefnt_block_initcalls = .; KEEP(*(.reliefnt.block_initcall)) "
                              "__stop_reliefnt_block_initcalls = .; "
                              "__start_reliefnt_subsys_initcalls = .; KEEP(*(.reliefnt.subsys_initcall)) "
                              "__stop_reliefnt_subsys_initcalls = .; "
                              "__start_reliefnt_initcalls = .; KEEP(*(.reliefnt.initcall)) "
                              "__stop_reliefnt_initcalls = .; } } INSERT AFTER .rodata;\n")
            binary = work / "initcalls"
            subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                            "tools/tests/initcalls_test.c", "kernel/init/initcalls.c",
                            "-Wl,-T," + str(script), "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
