#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run classic seccomp-BPF verifier/interpreter tests against production code."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="reliefnt-seccomp-bpf-", dir=temporary) as work:
    binary = Path(work) / "seccomp-bpf-test"
    subprocess.run([
        "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/seccomp_bpf_test.c",
        "kernel/seccomp/bpf.c",
        "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
