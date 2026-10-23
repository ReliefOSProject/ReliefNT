#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from kernel_test_sources import module_text

ROOT = Path(__file__).resolve().parents[1]


class ProcfsTests(unittest.TestCase):
    def test_real_procfs_directory_lookup_and_enumeration(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-procfs-") as directory:
            executable = str(Path(directory) / "procfs")
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                            "tools/tests/procfs_directories_test.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)


    def test_proc_pid_paths_start_after_proc_slash(self):
        source = (ROOT / "fs/proc/procfs.c").read_text()
        self.assertIn("const char *p = path + 6;", source)


    def test_procfs_exports_cpu_runtime_stats(self):
        source = (ROOT / "fs/proc/procfs.c").read_text()
        self.assertIn('proc_text_eq(path, "/proc/stat")', source)


    def test_pty_hangup_preserves_pending_canonical_input(self):
        source = module_text("pty")
        start = source.index("int pty_destroy(")
        end = source.index("/**", start)
        destroy = source[start:end]
        self.assertIn("pty_commit_canonical_input(session);", destroy)



if __name__ == "__main__":
    unittest.main()
