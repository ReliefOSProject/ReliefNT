#!/usr/bin/env python3
"""Run the two-interface ioctl and IPv4 routing fixture."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-multi-nic-") as work:
    binary = Path(work) / "net_multi_interface"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-fno-pie", "-no-pie", "-ffunction-sections",
                    "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/net_multi_interface_test.c", "net/interface/state.c",
                    "net/interface/routes.c", "net/interface/ioctl.c", "net/utility.c",
                    "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
