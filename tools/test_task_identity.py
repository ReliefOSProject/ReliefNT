#!/usr/bin/env python3
"""Exercise independently retained native PID identities with real ownership."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-task-identity-") as work:
    binary = str(Path(work) / "identity")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/task_identity_test.c",
                    "kernel/sched/identity.c", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], check=True, timeout=10)
