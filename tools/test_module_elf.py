#!/usr/bin/env python3
"""Exercise the real native ELF loader and relocation engine in a private host process."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ModuleELF(unittest.TestCase):
    def test_native_elf_and_failure_rollback(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-elf-") as directory:
            work = Path(directory)
            source = work / "fixture.c"
            source.write_text('extern int imported_function(int);\n'
                              'static volatile int zeroed[4100];\n'
                              'const char description[]="ReliefNT test";\n'
                              'int fixture_entry(int value) { return imported_function(value)+zeroed[4099]; }\n')
            obj = work / "fixture.o"
            subprocess.run(["clang", "--target=x86_64-elf", "-std=c11", "-O1",
                            "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-common",
                            "-mno-red-zone", "-mcmodel=large", "-fno-unwind-tables",
                            "-fno-asynchronous-unwind-tables", "-c", str(source), "-o", str(obj)], check=True)
            binary = work / "test"
            subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/module_elf_test.c",
                            "kernel/module/elf.c", "kernel/module/layout.c",
                            "arch/x86_64/module/relocate.c", "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary), str(obj)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
