#!/usr/bin/env python3
"""Build and run a host fixture against raw select and production poll code."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tools/tests/select_bitmap_test.c"


def main() -> int:
    temp_root = Path(os.environ.get("TMPDIR", str(ROOT / "out" / "tmp")))
    temp_root.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temp_root)
    try:
        with tempfile.TemporaryDirectory(prefix="select-bitmap-", dir=temp_root) as temp_name:
            binary = Path(temp_name) / "select_bitmap_test"
            command = [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-maybe-uninitialized",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-ffunction-sections",
                "-fdata-sections",
                str(FIXTURE),
                "-Wl,--gc-sections",
                "-o",
                str(binary),
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "include/uapi"),
            ]
            compiled = subprocess.run(
                command, cwd=ROOT, check=False, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            )
            if compiled.returncode:
                sys.stderr.write(compiled.stdout)
                return compiled.returncode
            completed = subprocess.run(
                [str(binary)], cwd=ROOT, check=False, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            )
            sys.stdout.write(completed.stdout)
            return completed.returncode
    except subprocess.TimeoutExpired as error:
        sys.stderr.write(f"select bitmap fixture timed out: {error.cmd}\n")
        if error.stdout:
            sys.stderr.write(error.stdout.decode() if isinstance(error.stdout, bytes) else error.stdout)
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
