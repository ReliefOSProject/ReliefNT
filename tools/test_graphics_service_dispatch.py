#!/usr/bin/env python3
"""Inspect permanent consumer ELF objects, rejecting direct optional DRM imports."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCES=("drivers/tty/pty/vt.c", "drivers/video/framebuffer/vmware.c", "drivers/video/svga/device.c",
         "fs/devfs/devfs.c", "fs/sysfs/sysfs.c", "kernel/exec/userland.c", "kernel/sched/exec.c",
         "kernel/signal/signal.c", "kernel/syscall/fd/descriptor.c", "kernel/syscall/entry.c")
with tempfile.TemporaryDirectory(prefix="reliefnt-graphics-service-dispatch-") as directory:
    remaining={}
    for i,source in enumerate(SOURCES):
        obj=Path(directory)/f"consumer-{i}.o"
        subprocess.run(["clang","--target=x86_64-elf","-std=gnu11","-O1","-ffreestanding",
                        "-fno-builtin","-Iinclude","-Iinclude/uapi","-c",source,"-o",str(obj)],cwd=ROOT,check=True)
        imports=subprocess.check_output(["llvm-nm","--undefined-only",str(obj)],text=True)
        direct=[line for line in imports.splitlines() if line.split()[-1].startswith("drm_")]
        if direct:remaining[source]=direct
    if remaining:raise AssertionError("Permanent objects still import DRM implementation: "+repr(remaining))
    print("PASS permanent console/VT/SVGA/devfs/sysfs/scheduler/signal/syscall ELF consumers have no direct DRM imports")
