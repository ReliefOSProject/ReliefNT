#!/usr/bin/env python3
"""Exercise VFS notification hooks against real tmpfs and fsnotify core."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path(os.environ.get("INOTIFY_TEST_OUT", ROOT.parents[1] / "out/inotify/vfs"))
OUTPUT.mkdir(parents=True, exist_ok=True)
binary = OUTPUT / "inotify-vfs-test"
subprocess.run([
    "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-Iinclude", "-Iinclude/uapi", "tools/tests/inotify_vfs_test.c",
    "fs/tmpfs/core.c", "mm/physical/proc.c", "fs/vfs/mount_tree.c", "fs/vfs/lookup_cache.c", "fs/vfs/dentry.c",
    "fs/vfs/file.c", "fs/vfs/mutation.c", "fs/vfs/attributes.c", "fs/vfs/notify.c", "fs/vfs/inode.c",
    "fs/notify/core.c", "fs/notify/queue.c", "fs/notify/watch.c", "fs/notify/limits.c",
    "-o", str(binary),
], cwd=ROOT, check=True)
subprocess.run([str(binary)], cwd=ROOT, timeout=30, check=True)
