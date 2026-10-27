#!/usr/bin/env python3
"""Prove permanent OFD/syscall dispatch has no direct sound-core link dependency."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("kernel/syscall/device/device_open.c", "kernel/syscall/device/device_io.c",
           "kernel/syscall/device/legacy_ioctl.c", "kernel/syscall/fd/descriptor.c",
           "kernel/syscall/event/poll.c", "kernel/syscall/io/legacy_io.c")
with tempfile.TemporaryDirectory(prefix="reliefnt-character-dispatch-") as directory:
    for i, source in enumerate(SOURCES):
        obj = Path(directory) / f"dispatch-{i}.o"
        subprocess.run(["clang", "--target=x86_64-elf", "-std=gnu11", "-O1", "-ffreestanding",
                        "-fno-builtin", "-Iinclude", "-Iinclude/uapi", "-c", source, "-o", str(obj)], cwd=ROOT, check=True)
        imports = subprocess.check_output(["llvm-nm", "--undefined-only", str(obj)], text=True)
        direct = [line for line in imports.splitlines() if line.split()[-1].startswith("audio_")]
        if direct:
            raise AssertionError(source + " still imports optional sound core directly:\n" + "\n".join(direct))
    print("PASS actual ELF character/OFD/syscall dispatch has no hard sound-core imports")
