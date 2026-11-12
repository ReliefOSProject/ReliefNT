#!/usr/bin/env python3
"""Exercise independent NIC identities against real service admission."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(root / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="reliefnt-netdev-", dir=temporary) as work:
    binary = Path(work) / "netdev"
    subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/net_device_test.c", "kernel/device/services.c",
                    "kernel/device/driver_manager_phase.c", "-o", str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
