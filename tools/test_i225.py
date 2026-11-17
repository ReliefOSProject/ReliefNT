#!/usr/bin/env python3
"""Exercise the native I225-V driver against a PCI/MMIO/DMA boundary model."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["tools/tests/i225_test.c", *[
    "drivers/net/i225/" + name + ".c" for name in ("main", "hardware", "rings")]]


def main():
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefnt-i225-", dir=temporary) as directory:
        binary = Path(directory) / "i225"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                        *SOURCES, "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    main()
