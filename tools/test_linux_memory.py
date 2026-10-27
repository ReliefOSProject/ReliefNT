#!/usr/bin/env python3
"""Exercise kernel memory implementation with host physical-page fixtures."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from kernel_test_sources import kernel_sources, module_text

ROOT = Path(__file__).resolve().parents[1]


class LinuxMemoryTests(unittest.TestCase):
    def test_mprotect_linux_reference(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-mprotect-reference-") as tmp:
            executable = str(Path(tmp) / "mprotect-runtime")
            subprocess.run(["cc", "-std=c11", "-O1", "-pthread",
                            "tools/tests/mprotect_runtime_probe.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=30)

    def test_jit_memory_linux_reference(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-jit-reference-") as tmp:
            executable = str(Path(tmp) / "jit-memory-runtime")
            subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                            "tools/tests/jit_memory_runtime_probe.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=30)

    def test_memory_ownership_and_elf_page_boundaries(self):
        cases = (("physical_pages", []), ("paging_protection", []),
                 ("private_anon_fault", ["kernel/exec/usercopy.c"]), ("elf_interpreter", []),
                 ("elf_sparse_reservation", ["fs/vfs/mount_tree.c","fs/vfs/file.c", "fs/vfs/open.c","fs/vfs/attributes.c","fs/vfs/exec_file.c"]),
                 ("elf_file_page", ["mm/cache/page_cache.c","fs/vfs/mount_tree.c","fs/vfs/file.c", "fs/vfs/open.c","fs/vfs/attributes.c","fs/vfs/exec_file.c"]), ("user_mmap_arena", []),
                 ("mprotect_vma_growth", [*kernel_sources("scheduler"),"kernel/device/character.c","fs/vfs/file.c", "fs/vfs/open.c","fs/vfs/attributes.c","fs/vfs/exec_file.c"]),
                 ("brk_collision", [*kernel_sources("user_memory"),"fs/vfs/mount_tree.c","fs/vfs/file.c", "fs/vfs/open.c","fs/vfs/attributes.c"]))
        for name, sources in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="reliefos-mm-") as tmp:
                executable = str(Path(tmp) / name)
                subprocess.run([
                    "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections",
                    "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                    f"tools/tests/{name}_test.c", *sources, "-o", executable,
                ], cwd=ROOT, check=True)
                subprocess.run([executable], cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
