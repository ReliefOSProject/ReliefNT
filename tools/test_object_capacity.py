#!/usr/bin/env python3
"""Exercise live object-table growth and generation checks in production code."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(root / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="object-capacity-", dir=temporary) as work:
    binary = str(Path(work) / "objects")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                    "-fsanitize=address,undefined", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/object_capacity_test.c", "fs/vfs/object.c", "-o", binary],
                   cwd=root, check=True)
    subprocess.run([binary], check=True, timeout=20)
