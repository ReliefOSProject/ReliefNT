#!/usr/bin/env python3
"""Exercise resource-backed pipe creation and Linux capacity controls."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(root / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="pipe-capacity-", dir=temporary) as work:
    binary = str(Path(work) / "pipes")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-O1",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-fsanitize=address,undefined", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/pipe_capacity_test.c", "fs/vfs/object.c", "-o", binary],
                   cwd=root, check=True)
    subprocess.run([binary], check=True, timeout=20)
