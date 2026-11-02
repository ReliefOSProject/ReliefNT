#!/usr/bin/env python3
"""Exercise production PRIO_PROCESS dispatch without renicing host processes."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="process-priority-", dir=os.environ["TMPDIR"]) as directory:
    binary = str(Path(directory) / "priority")
    subprocess.run(["cc", "-std=c11", "-O2", "-g", "-flto", "-fwhole-program",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/process_priority_test.c", "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=20)
