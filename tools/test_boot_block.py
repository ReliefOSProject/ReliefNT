#!/usr/bin/env python3
"""Exercise reserved boot-image block I/O without copying the huge payload."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-boot-block-") as directory:
    binary = Path(directory) / "boot-block"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/boot_block_test.c", "block/ramdisk/boot.c",
        "block/device/registry.c", "kernel/module/lifecycle.c", "kernel/params/value.c", "kernel/module/layout.c", "kernel/module/symbols.c",
        "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=15)
