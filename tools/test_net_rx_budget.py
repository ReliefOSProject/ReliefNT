#!/usr/bin/env python3
"""Exercise task-context IRQ receive budgets using production network input."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="reliefnt-net-rx-", dir=temporary) as work:
    binary = Path(work) / "net-rx"
    subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/net_rx_budget_test.c",
                    "net/input.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
