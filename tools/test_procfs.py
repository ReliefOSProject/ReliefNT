#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProcfsTests(unittest.TestCase):
    def test_real_procfs_directory_lookup_and_enumeration(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-procfs-") as directory:
            executable = str(Path(directory) / "procfs")
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                            "tools/tests/procfs_directories_test.c", "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c",
                            "fs/vfs/path.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/pseudo.c", "fs/vfs/attributes.c", "fs/proc/mount.c",
                            "kernel/sched/identity.c", "fs/security/access.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)


    def test_proc_pid_paths_start_after_proc_slash(self):
        source = (ROOT / "fs/proc/procfs.c").read_text()
        self.assertIn("const char *p = path + 6;", source)


    def test_procfs_exports_cpu_runtime_stats(self):
        source = (ROOT / "fs/proc/procfs.c").read_text()
        self.assertIn('proc_text_eq(path, "/proc/stat")', source)


# PTY canonical input after hangup is exercised by linux_pty_test.c in the
# explicit linux_pty suite; procfs no longer searches a header prototype for it.
if __name__ == "__main__":
    unittest.main()
