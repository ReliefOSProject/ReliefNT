#!/usr/bin/env python3
"""Ensure permanent VM/scheduler objects do not import the optional PCM/DRM cores."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("mm/user/mapping.c", "mm/user/protect.c", "mm/user/vma.c",
           "kernel/sched/tables.c", "kernel/sched/clone.c")
with tempfile.TemporaryDirectory(prefix="reliefnt-device-mapping-dispatch-") as directory:
    remaining = {}
    for i, source in enumerate(SOURCES):
        obj = Path(directory) / f"mapping-{i}.o"
        subprocess.run(["clang", "--target=x86_64-elf", "-std=gnu11", "-O1", "-ffreestanding",
                        "-fno-builtin", "-Iinclude", "-Iinclude/uapi", "-c", source, "-o", str(obj)], cwd=ROOT, check=True)
        imports = subprocess.check_output(["llvm-nm", "--undefined-only", str(obj)], text=True)
        direct = [line for line in imports.splitlines() if line.split()[-1].startswith(("audio_", "drm_"))]
        if direct:
            remaining[source] = direct
    if remaining:
        raise AssertionError("Permanent VM objects still import optional device code: " + repr(remaining))
    print("PASS actual VM/scheduler ELF objects use provider-independent device mapping lifetime")
