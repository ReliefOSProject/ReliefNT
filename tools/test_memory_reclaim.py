#!/usr/bin/env python3
"""Exercise production cache, file fault/protect and ext mutation ownership."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MemoryReclaimTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang")
        if not compiler:
            raise RuntimeError("clang is required for these sanitizer fixtures")
        cls.temp = tempfile.TemporaryDirectory(prefix="memory-reclaim-", dir=os.environ["TMPDIR"])
        common = [compiler, "-std=c11", "-pthread", "-O1", "-g", "-Wall", "-Wextra",
                  "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-no-pie", "-ffunction-sections", "-fdata-sections",
                  "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi"]
        cls.binaries = {}
        cases = {
            "cache": (["tools/tests/memory_reclaim_test.c", "mm/cache/page_cache.c",
                       "kernel/sync/lock.c"], ["-Werror"]),
            "boundary": (["tools/tests/page_cache_ref_boundary_test.c", "kernel/sync/lock.c"],
                         ["-Werror"]),
            "fallocate": (["tools/tests/cache_fallocate_test.c", "mm/cache/page_cache.c",
                           "kernel/sync/lock.c"], ["-Wno-unused-function", "-mllvm", "-asan-globals=0"]),
            "fault": (["tools/tests/elf_file_page_test.c", "mm/cache/page_cache.c",
                       "fs/vfs/mount_tree.c", "fs/vfs/file.c", "fs/vfs/attributes.c",
                       "fs/vfs/exec_file.c"], ["-Werror"]),
            "protect": (["tools/tests/private_fault_dispatch_test.c", "arch/x86_64/interrupt/idt.c",
                          "kernel/sched/core.c", "mm/cache/page_cache.c", "fs/vfs/mount_tree.c",
                          "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c",
                          "fs/vfs/exec_file.c", "-Wl,--wrap=vfs_exec_source_get",
                          "-Wl,--wrap=vfs_exec_source_put"], []),
        }
        for name, (sources, flags) in cases.items():
            binary = str(Path(cls.temp.name) / name)
            subprocess.run([*common, *flags, *sources, "-o", binary], cwd=ROOT, check=True)
            cls.binaries[name] = binary
        for name in ("mm/cache/page_cache.c", "mm/user/fault.c", "mm/user/protect.c",
                     "fs/ext_common/native.c"):
            print("SOURCE", name, hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), flush=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, binary, name=None, retained=False):
        env = dict(os.environ)
        # These processes deliberately leave dirty/unknown or saturated frames
        # resident; freeing them solely for leak checking would break the contract.
        if retained:
            env["ASAN_OPTIONS"] = "detect_leaks=0"
        args = [self.binaries[binary]] + ([name] if name else [])
        subprocess.run(args, cwd=ROOT, env=env, check=True, timeout=45)

    def test_cache_lifetime_budget_and_failures(self):
        for case in ("exhaustion", "exhaustion-fail", "hit-no-loader", "irq", "budget", "metadata", "dirty",
                     "overflow", "retired-overflow", "loader", "pin", "pin-fail", "pressure-pin", "cluster-identity", "growth-failure"):
            with self.subTest(case=case):
                self.run_case("cache", case, case in ("budget", "dirty", "overflow"))

    def test_reference_saturation(self):
        self.run_case("boundary", retained=True)

    def test_shared_write_fault(self):
        self.run_case("fault", "shared-writable", retained=True)
        self.run_case("fault", "private-cow")
        self.run_case("fault", "clean-reload")

    def test_shared_write_protection(self):
        self.run_case("protect", "shared-mprotect", retained=True)
        self.run_case("protect", "shared-growth", retained=True)
        self.run_case("protect", "shared-partial-growth", retained=True)

    def test_fallocate_and_inode_retirement(self):
        for case in ("growth", "zero", "punch", "failure", "stat-failure", "terminal",
                     "terminal-fail", "size-aliases", "error-growth", "error-growth-zero",
                     "error-growth-no-commit", "error-growth-read-fail", "growth-stat-fail", "error-growth-smaller",
                     "error-growth-generation", "error-growth-known-size", "error-growth-independent-stat",
                     "independent-stat-shrink"):
            with self.subTest(case=case):
                self.run_case("fallocate", case, case != "terminal")


if __name__ == "__main__":
    unittest.main(verbosity=2)
