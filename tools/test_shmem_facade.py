#!/usr/bin/env python3
"""Exercise anonymous tmpfs files through the production VFS facade."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(root / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="shmem-facade-", dir=temporary) as work:
    binary = str(Path(work) / "facade")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/shmem_facade_test.c",
                    "fs/tmpfs/core.c", "lib/text_utf16.c", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], check=True, timeout=20)
