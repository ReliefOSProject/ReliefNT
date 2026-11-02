#!/usr/bin/env python3
"""Exercise the production inotify read adapter with bounded fault injection."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-inotify-adapter-") as directory:
    binary = Path(directory) / "inotify-adapter-test"
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-DCONFIG_SWAP", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/inotify_adapter_test.c",
        "kernel/syscall/event/inotify.c", "-Wl,--gc-sections", "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
