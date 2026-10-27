#!/usr/bin/env python3
"""Exercise native mount resolution, DAC, creation policy and retained chroot."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c", "fs/vfs/path.c",
    "fs/vfs/dentry.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c",
    "fs/vfs/mutation.c", "fs/security/access.c", "kernel/syscall/fs/create.c",
    "kernel/syscall/fs/mutation.c",
]
with tempfile.TemporaryDirectory(prefix="reliefnt-permissions-") as directory:
    for name in ("linux_permissions", "chroot_paths"):
        executable = str(Path(directory) / name)
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
            f"tools/tests/{name}_test.c", *SOURCES, "-o", executable,
        ], cwd=ROOT, check=True)
        subprocess.run([executable], cwd=ROOT, check=True, timeout=30)
