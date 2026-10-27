#!/usr/bin/env python3
"""Exercise production CPU rendezvous with deferred IPIs and concurrent callers."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="reliefnt-smp-barrier-") as directory:
        for name in ("smp_barrier", "membarrier_cpu"):
            binary = Path(directory) / name
            subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/" + name + "_test.c",
                            "arch/x86_64/cpu/barrier.c", "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], check=True, timeout=10,
                           env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})



if __name__ == "__main__":
    main()
