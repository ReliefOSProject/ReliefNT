#!/usr/bin/env python3
"""Prove standard DRM file operations are invoked through retained generic OFDs."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("kernel/syscall/device/device_open.c", "kernel/syscall/fd/descriptor.c",
           "kernel/syscall/event/poll.c", "kernel/syscall/io/io.c",
           "kernel/syscall/io/legacy_io.c", "kernel/syscall/io/vector.c", "kernel/syscall/dispatch.c")
with tempfile.TemporaryDirectory(prefix="reliefnt-drm-file-dispatch-") as directory:
    remaining = {}
    for i, source in enumerate(SOURCES):
        obj = Path(directory) / f"dispatch-{i}.o"
        subprocess.run(["clang", "--target=x86_64-elf", "-std=gnu11", "-O1", "-ffreestanding",
                        "-fno-builtin", "-Iinclude", "-Iinclude/uapi", "-c", source, "-o", str(obj)], cwd=ROOT, check=True)
        imports = subprocess.check_output(["llvm-nm", "--undefined-only", str(obj)], text=True)
        direct = [line for line in imports.splitlines() if line.split()[-1].startswith(("drm_file_", "drm_prime_file_", "drm_fd_", "drm_event_", "syscall_drm_"))]
        if direct:
            remaining[source] = direct
    if remaining:
        raise AssertionError("Permanent file objects still import DRM implementations: " + repr(remaining))
    print("PASS actual standard OFD/syscall ELF objects have no direct DRM file-operation imports")
