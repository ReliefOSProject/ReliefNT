#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise real RTL8139 objects at the PCI/port-I/O/DMA hardware boundary."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["tools/tests/rtl8139_test.c", *[
    "drivers/net/rtl8139/" + name + ".c" for name in ("main", "hardware", "rings", "io")]]


def main():
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefnt-rtl8139-", dir=temporary) as directory:
        binary = Path(directory) / "rtl8139"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                        *SOURCES, "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
