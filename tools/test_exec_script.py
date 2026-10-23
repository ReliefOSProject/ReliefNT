#!/usr/bin/env python3
"""Exercise production script parsing and argv updates under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-exec_script-") as directory:
    binary = Path(directory) / "exec-script-test"
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude", "-Iinclude/uapi",
        "-Iinclude", "-Wl,--gc-sections",
        "tools/tests/exec_script_test.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
