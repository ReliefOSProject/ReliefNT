#!/usr/bin/env python3
"""Exercise real inotify sysctls against the notification accounting core."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-inotify-proc-") as directory:
    binary = Path(directory) / "inotify-proc"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/inotify_proc_test.c",
        "fs/proc/inotify.c", "fs/notify/core.c", "fs/notify/queue.c",
        "fs/notify/watch.c", "fs/notify/limits.c", "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=45)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
