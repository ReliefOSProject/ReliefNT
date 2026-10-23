#!/usr/bin/env python3
"""Exercise actual descriptor helpers and scheduler table growth with sanitizers."""
from pathlib import Path
import subprocess
import tempfile

from kernel_test_sources import kernel_sources, module_text

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-descriptors-") as directory:
    binary = str(Path(directory) / "descriptors")
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude", "-Wl,--gc-sections",
        "tools/tests/descriptor_table_test.c", *kernel_sources("scheduler"),
        "kernel/sync/wait.c", "kernel/syscall/ipc/sysv_msg.c",
        "kernel/syscall/ipc/sysv_sem.c", "kernel/syscall/fs/locks.c",
        "sound/core/core.c", *kernel_sources("pcm"),
        "sound/core/device.c", "sound/core/alsa_control.c",
        "sound/core/mixer.c", "sound/core/oss.c",
        "sound/core/alsa_pcm.c",
        "sound/core/timer.c",
        "fs/vfs/object.c",
        "-o", binary,
    ], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=20)
