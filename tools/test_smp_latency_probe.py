#!/usr/bin/env python3
"""Exercise the native Linux/POSIX probe with small, finite host workloads."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools/tests/smp_latency_probe.c"


class NativeProbe(unittest.TestCase):
    def test_small_run_checks_all_fault_types_and_reports_exact_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "probe"
            built = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                    "-pthread", str(SOURCE), "-o", str(binary)],
                                   text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary), "--workers", "2", "--pages", "4", "--rounds", "2",
                                     "--directory", directory], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            signatures = re.search(r"^NATIVE_PATTERN page0=(\d+) page1=(\d+) page256=(\d+)$", result.stdout, re.M)
            self.assertIsNotNone(signatures, "file pages must expose distinct content signatures")
            self.assertEqual(len(set(signatures.groups())), 3)
            rows = re.findall(r"^NATIVE kind=(\w+) count=(\d+) p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+)$",
                              result.stdout, re.M)
            self.assertEqual({row[0] for row in rows}, {"demand", "cow", "shared", "filemap", "syscall"})
            self.assertEqual(len(rows), 5)
            for _, count, p50, p95, p99, maximum in rows:
                self.assertEqual(int(count), 16)
                self.assertLessEqual(int(p50), int(p95))
                self.assertLessEqual(int(p95), int(p99))
                self.assertLessEqual(int(p99), int(maximum))

    def test_invalid_or_unbounded_parameters_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "probe"
            built = subprocess.run(["cc", "-std=c11", "-O2", "-pthread", str(SOURCE), "-o", str(binary)],
                                   text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            for args in (("--workers", "0"), ("--pages", "999999"), ("--rounds", "0")):
                self.assertNotEqual(subprocess.run([str(binary), *args], capture_output=True).returncode, 0)

    def test_unshared_child_sample_writes_cannot_report_success(self):
        shim = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/types.h>
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    if ((flags & MAP_SHARED) && (flags & MAP_ANONYMOUS)) flags = (flags & ~MAP_SHARED) | MAP_PRIVATE;
    return real_mmap(addr, len, prot, flags, fd, offset);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary, source, preload = (Path(directory) / name for name in ("probe", "mmap.c", "mmap.so"))
            source.write_text(shim)
            subprocess.run(["cc", "-std=c11", "-O2", "-pthread", str(SOURCE), "-o", str(binary)], check=True)
            subprocess.run(["cc", "-shared", "-fPIC", str(source), "-ldl", "-o", str(preload)], check=True)
            result = subprocess.run([str(binary), "--workers", "2", "--pages", "4", "--rounds", "1",
                                     "--directory", directory], env={**os.environ, "LD_PRELOAD": str(preload)},
                                    text=True, capture_output=True, timeout=20)
            self.assertNotEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
