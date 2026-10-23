#!/usr/bin/env python3
"""Run evdev recovery, absolute frame and keyboard LED production checks."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="input-routing-") as temporary:
    binary = Path(temporary) / "input"
    subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/input_routing_fixture.c", "tools/tests/input_routing_test.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
