#!/usr/bin/env python3
"""Exercise shared high module mappings without privileged host operations."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ModuleMemory(unittest.TestCase):
    def test_shared_mapping_and_alias_permissions(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-memory-") as directory:
            binary = Path(directory) / "test"
            subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-DCONFIG_MODULES=1",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/module_memory_test.c",
                            "arch/x86_64/module/memory.c", "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_module_addresses_are_not_dma_physical_addresses(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-no-dma-") as directory:
            work = Path(directory)
            source = work / "probe.c"
            source.write_text('#include <assert.h>\n#include <reliefnt/mm.h>\n'
                              '#include <reliefnt/module_memory.h>\nint main(void) {\n'
                              'assert(!mm_virt_to_phys((void *)RELIEFNT_MODULE_BASE));\n'
                              'assert(!mm_virt_to_phys((void *)(RELIEFNT_MODULE_END-1)));\n}\n')
            binary = work / "test"
            subprocess.run(["clang", "-std=c11", "-O1", "-ffunction-sections", "-fdata-sections",
                            "-Wl,--gc-sections", "-Iinclude", str(source),
                            "mm/physical/mm.c", "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
