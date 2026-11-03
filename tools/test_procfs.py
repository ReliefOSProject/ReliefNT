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
            for fixture in ("procfs_directories_test", "procfs_views_test"):
                subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                                "-Iinclude", "-Iinclude/uapi",
                                f"tools/tests/{fixture}.c", "fs/vfs/mount_tree.c", "fs/vfs/lookup_cache.c", "fs/vfs/mount_table.c",
                                "fs/vfs/path.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/pseudo.c",
                                "fs/vfs/attributes.c", "fs/proc/mount.c", "fs/proc/views.c",
                                "kernel/syscall/fd/proc.c", "fs/vfs/exec_file.c",
                                "kernel/sched/identity.c", "fs/security/access.c", "-o", executable],
                               cwd=ROOT, check=True)
                subprocess.run([executable], check=True, timeout=10)


    def test_native_linux_reference(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-proc-reference-") as directory:
            executable = str(Path(directory) / "proc-reference")
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-pthread", "tools/tests/procfs_probe.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_proc_pid_paths_start_after_proc_slash(self):
        source = (ROOT / "fs/proc/procfs.c").read_text()
        self.assertIn("const char *p = path + 6;", source)

    def test_freestanding_linux_reference(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-proc-raw-") as directory:
            executable = str(Path(directory) / "proc-raw")
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdlib",
                            "-static", "-no-pie", "-Wl,-e,_start", "-Wl,-Ttext-segment=0x400000",
                            "-Iinclude/uapi", "tools/tests/procfs_raw_probe.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)


# PTY canonical input after hangup is exercised by linux_pty_test.c in the
# explicit linux_pty suite; procfs no longer searches a header prototype for it.
if __name__ == "__main__":
    unittest.main()
