#!/usr/bin/env python3
"""Run the production directory-bound FAT metadata codec with sanitizers."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-sidecar-") as directory:
    binary = Path(directory) / "sidecar-directories"
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/sidecar_directory_runtime_test.c",
        "tools/tests/sidecar_directory_backend.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
