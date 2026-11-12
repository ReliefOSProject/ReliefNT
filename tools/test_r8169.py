#!/usr/bin/env python3
"""Exercise Realtek driver objects using a PCI/MMIO/DMA boundary model."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODEL_SOURCES = ["tools/tests/r8169_test.c", *[
    "drivers/net/r8169/" + name + ".c"
    for name in ("main", "hardware", "rings", "rtl8168", "rtl8125", "firmware_8125")]]
IO_SOURCES = ["tools/tests/r8169_io_test.c", "drivers/net/r8169/io.c"]


def main():
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefnt-r8169-", dir=temporary) as directory:
        for name, sources in (("r8169", MODEL_SOURCES), ("r8169-io", IO_SOURCES)):
            binary = Path(directory) / name
            subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                            *sources, "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
