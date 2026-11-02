#!/usr/bin/env python3
"""Exercise production scheduler wakeups and reschedule IPI boundaries."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    temporary = Path(os.environ.get("TMPDIR", ROOT / "out/tmp"))
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sched-wakeup-", dir=temporary) as directory:
        for name in ("sched_wakeup", "smp_reschedule", "sched_clock"):
            binary = Path(directory) / name
            subprocess.run(["clang", "-std=c11", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude/uapi", f"tools/tests/{name}_test.c",
                "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)


if __name__ == "__main__":
    main()
