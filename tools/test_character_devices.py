#!/usr/bin/env python3
"""Exercise real character registration, per-open callbacks and module unload pins."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-character-devices-") as directory:
    binary = Path(directory) / "devices"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-fno-pie", "-no-pie",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/character_devices_test.c",
                    "kernel/device/character.c", "kernel/module/lifecycle.c", "kernel/module/layout.c",
                    "kernel/module/symbols.c", "kernel/params/value.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
