#!/usr/bin/env python3
"""Check VMA backing/provider lifetime independently of a character OFD."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-device-mappings-") as directory:
    binary = Path(directory) / "mapping"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-fno-pie", "-no-pie",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/device_mappings_test.c",
                    "kernel/device/mapping.c", "kernel/module/lifecycle.c", "kernel/module/layout.c",
                    "kernel/module/symbols.c", "kernel/params/value.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
