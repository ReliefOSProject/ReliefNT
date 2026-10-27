#!/usr/bin/env python3
"""Prove permanent objects invoke the optional sound core through pinned services."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("fs/devfs/devfs.c", "fs/devfs/identity.c", "fs/proc/procfs.c",
           "kernel/device/audio_work.c", "kernel/device/api.c", "kernel/device/registry.c", "kernel/device/driver_manager_phase.c",
           "kernel/time/time.c")
with tempfile.TemporaryDirectory(prefix="reliefnt-sound-service-dispatch-") as directory:
    remaining = {}
    for i, source in enumerate(SOURCES):
        obj = Path(directory) / f"dispatch-{i}.o"
        subprocess.run(["clang", "--target=x86_64-elf", "-std=gnu11", "-O1", "-ffreestanding",
                        "-fno-builtin", "-Iinclude", "-Iinclude/uapi", "-c", source, "-o", str(obj)], cwd=ROOT, check=True)
        imports = subprocess.check_output(["llvm-nm", "--undefined-only", str(obj)], text=True)
        direct = [line for line in imports.splitlines() if line.split()[-1].startswith(("audio_", "storage_audio_"))]
        if direct:
            remaining[source] = direct
    if remaining:
        raise AssertionError("Permanent objects still import optional sound implementations: " + repr(remaining))
    print("PASS actual permanent inventory/IRQ/driver-manager ELF objects have no direct sound implementation imports")
