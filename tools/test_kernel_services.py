#!/usr/bin/env python3
"""Exercise real optional service publication and callback/module teardown."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-kernel-services-") as directory:
    binary = Path(directory) / "devices"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-fno-pie", "-no-pie",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/kernel_services_test.c",
                    "kernel/device/service.c", "kernel/module/lifecycle.c", "kernel/module/layout.c",
                    "kernel/module/symbols.c", "kernel/params/value.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
