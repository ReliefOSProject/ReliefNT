#!/usr/bin/env python3
"""Compare the private FAT/exFAT calendar codec with the independent host calendar."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="reliefnt-dos-time-") as directory:
        binary = Path(directory) / "dos-time"
        subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-Iinclude", "-Iinclude/uapi", "tools/tests/dos_time_test.c", "-o", str(binary)],
                       cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
