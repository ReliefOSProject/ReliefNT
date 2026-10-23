#!/usr/bin/env python3
"""Run anonymous-file capacity, sparse backing and sealing regressions."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(root / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="memfd-resources-", dir=temporary) as work:
    binary = str(Path(work) / "memfd")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-O1",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-fsanitize=address,undefined", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/memfd_resources_test.c", "fs/vfs/object.c", "fs/tmpfs/core.c", "-o", binary],
                   cwd=root, check=True)
    results = [subprocess.run([binary, *args], timeout=20).returncode for args in ([], ["size"])]
    syscall_binary = str(Path(work) / "memfd-syscall")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-function",
                    "-g", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-fsanitize=address,undefined", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/memfd_syscall_error_test.c", "-o", syscall_binary],
                   cwd=root, check=True)
    results.append(subprocess.run([syscall_binary], timeout=20).returncode)
    raise SystemExit(1 if any(results) else 0)
