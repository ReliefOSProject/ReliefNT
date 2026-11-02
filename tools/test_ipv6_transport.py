#!/usr/bin/env python3
"""Run the focused IPv6 socket transport host fixture and IPv4 regression."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
out = Path(os.environ.get("RELIEFNT_TEST_OUT", root / "out/ipv6/transport")).resolve()
out.mkdir(parents=True, exist_ok=True)
binary = out / "ipv6_transport_test"
subprocess.run(["cc", "-std=c11", "-g", "-O1", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/ipv6_transport_test.c", "-o", str(binary)], cwd=root, check=True)
subprocess.run([str(binary)], cwd=root, check=True, timeout=20)
binary = out / "ipv6_vector_test"
subprocess.run(["clang", "-std=c11", "-g", "-O1", "-DCONFIG_SWAP",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/ipv6_vector_test.c", "tools/tests/ipv6_vector_stubs.c",
                "kernel/syscall/io/vector.c", "kernel/syscall/ipc/socket.c",
                "kernel/syscall/fs/lookup_state.c", "-o", str(binary)], cwd=root, check=True)
subprocess.run([str(binary)], cwd=root, check=True, timeout=20)
binary = out / "ipv6_tcp_test"
subprocess.run(["cc", "-std=c11", "-g", "-O1", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/ipv6_tcp_test.c", "net/tcp/input.c", "net/tcp/output.c",
                "net/tcp/socket.c", "net/utility.c", "-o", str(binary)], cwd=root, check=True)
subprocess.run([str(binary)], cwd=root, check=True, timeout=20)
