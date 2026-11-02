#!/usr/bin/env python3
"""Exercise the kernel route-netlink IPv6 control plane on the host."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
out = Path(os.environ.get("RELIEFNT_TEST_OUT", os.environ.get("IPV6_CONTROL_OUT", root / "out/ipv6/control"))).resolve()
out.mkdir(parents=True, exist_ok=True)
binary = out / "ipv6_control_test"
subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/ipv6_control_test.c", "-o", str(binary)], cwd=root, check=True)
subprocess.run([str(binary)], cwd=root, check=True, timeout=20)
