#!/usr/bin/env python3
"""Compile and run the Linux inotify ABI probe against the host kernel."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefnt-inotify-") as directory:
    work = Path(directory)
    binary = work / "inotify_runtime_probe"
    command = shlex.split(os.environ.get("CC", "cc"))
    command += [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        str(ROOT / "tools/tests/inotify_runtime_probe.c"), "-o", str(binary),
    ]
    subprocess.run(command, cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(binary), str(work / "probe")], cwd=ROOT, check=True, timeout=45)
