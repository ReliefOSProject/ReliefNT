#!/usr/bin/env python3
"""Exercise real builtin __param records with MODULES disabled and sysfs policy."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-builtin-parameters-") as directory:
    binary = Path(directory) / "parameters"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", '-DKBUILD_MODNAME="test_params"',
                    "-Iinclude", "-Iinclude/uapi", "tools/tests/builtin_parameters_test.c",
                    "kernel/params/value.c", "kernel/params/arguments.c", "kernel/params/builtin.c",
                    "kernel/params/registry.c", "fs/sysfs/parameters.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)
