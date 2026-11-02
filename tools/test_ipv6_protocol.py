#!/usr/bin/env python3
"""Build and run the focused IPv6 packet/state fixture in the task output tree."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
output = Path(os.environ.get("RELIEFNT_TEST_OUT", root / "out/ipv6/protocol")).resolve()
output.mkdir(parents=True, exist_ok=True)
source = root / "net" / "ipv6" / "core.c"
cmd = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
       "-I" + str(root / "include"), "-I" + str(root / "include/uapi"),
       str(root / "tools/tests/ipv6_protocol_test.c")]
if source.exists():
    cmd += [str(source), str(root / "net/link/ethernet.c"), str(root / "net/input.c")]
else:
    cmd += ["-DIPV6_NO_IMPLEMENTATION"]
cmd += ["-o", str(output / "ipv6_protocol_test")]
subprocess.run(cmd, check=True)
subprocess.run([str(output / "ipv6_protocol_test")], check=True)
