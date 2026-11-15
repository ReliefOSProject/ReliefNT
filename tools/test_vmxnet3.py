#!/usr/bin/env python3
"""Exercise VMXNET3 production objects at the modeled PCI/MMIO/DMA boundary."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["tools/tests/vmxnet3_test.c", *[
    "drivers/net/vmxnet3/" + name + ".c" for name in ("main", "hardware", "rings", "interrupt")]]


def main():
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefnt-vmxnet3-", dir=temporary) as directory:
        binary = Path(directory) / "vmxnet3"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                        *SOURCES, "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    main()
