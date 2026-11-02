#!/usr/bin/env python3
"""Build the real inotify watch and event core with sanitizers and exercise it."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path(os.environ.get("INOTIFY_TEST_OUT", ROOT.parents[1] / "out/inotify/core"))
OUTPUT.mkdir(parents=True, exist_ok=True)
BINARY = OUTPUT / "inotify-core-test"
subprocess.run([
    "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-Iinclude", "-Iinclude/uapi", "tools/tests/inotify_core_test.c",
    "fs/notify/core.c", "fs/notify/queue.c", "fs/notify/watch.c",
    "fs/notify/limits.c", "-o", str(BINARY),
], cwd=ROOT, check=True)
subprocess.run([str(BINARY)], cwd=ROOT, timeout=30, check=True)
