#!/usr/bin/env python3
"""Exercise deferred work with real execution/queue locks and module pins."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="workqueue-", dir=temporary) as directory:
    binary = Path(directory) / "workqueue"
    source = ROOT / "kernel/workqueue/core.c"
    extra = [str(source)] if source.exists() else ["-DWORKQUEUE_MISSING"]
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-pthread", "-DCONFIG_MODULES",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/workqueue_test.c", "kernel/sync/lock.c",
        *extra, "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=30)

with tempfile.TemporaryDirectory(prefix="workers-", dir=temporary) as directory:
    binary = Path(directory) / "workers"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/workqueue_workers_test.c",
        "kernel/workqueue/worker.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=10)
