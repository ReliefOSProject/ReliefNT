#!/usr/bin/env python3
"""Production gate and collector tests with deterministic per-thread clocks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefnt-lock-profile-") as directory:
    binary = str(Path(directory) / "profile")
    sources = ["tools/tests/lock_profile_test.c", "kernel/sync/lock.c"]
    if (ROOT / "kernel/sync/profile.c").exists():
        sources.append("kernel/sync/profile.c")
    subprocess.run(["cc", "-std=c11", "-pthread", "-O1", "-g", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-DCONFIG_LOCK_STAT", "-DRELIEFNT_LOCK_PROFILE_TEST",
                    "-Iinclude", "-Iinclude/uapi", *sources, "-o", binary],
                   cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=30)

with tempfile.TemporaryDirectory(prefix="reliefnt-lock-proc-") as directory:
    binary = str(Path(directory) / "proc")
    sources = ["tools/tests/lock_profile_proc_test.c", "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c",
               "fs/vfs/path.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/pseudo.c", "fs/vfs/attributes.c",
               "fs/proc/mount.c", "kernel/sched/identity.c", "fs/security/access.c"]
    if (ROOT / "kernel/sync/profile.c").exists():
        sources.append("kernel/sync/profile.c")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-DCONFIG_LOCK_STAT", "-DRELIEFNT_LOCK_PROFILE_TEST",
                    "-Iinclude", "-Iinclude/uapi", *sources, "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=10)

with tempfile.TemporaryDirectory(prefix="reliefnt-lock-cpu-") as directory:
    binary = str(Path(directory) / "cpu")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-DCONFIG_LOCK_STAT", "-DRELIEFNT_LOCK_PROFILE_TEST",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/lock_profile_cpu_lookup_test.c",
                    "kernel/sync/lock.c", "kernel/sync/profile.c", "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=10)

with tempfile.TemporaryDirectory(prefix="reliefnt-lock-sync-") as directory:
    binary = str(Path(directory) / "sync")
    subprocess.run(["cc", "-std=c11", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-DCONFIG_LOCK_STAT", "-DRELIEFNT_LOCK_PROFILE_TEST",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/lock_profile_sync_io_test.c",
                    "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c", "fs/vfs/path.c", "fs/vfs/file.c",
                    "fs/vfs/open.c", "fs/vfs/pseudo.c", "fs/vfs/attributes.c", "fs/proc/mount.c",
                    "kernel/sched/identity.c", "fs/security/access.c", "kernel/sync/profile.c",
                    "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=10)
