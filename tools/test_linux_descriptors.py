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
        "sound/core/timer.c", "sound/core/mapping.c", "sound/core/services.c", "sound/core/work.c", "sound/core/storage_audio.c",
        "kernel/device/service.c", "kernel/device/graphics_service.c", "kernel/device/sound_service.c", "kernel/device/driver_manager_phase.c",
        "fs/devfs/identity.c",
        "fs/vfs/object.c", "fs/vfs/write_access.c", "fs/vfs/path.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c", "fs/vfs/initial.c", "fs/security/access.c", "kernel/syscall/ipc/unix_name.c",
        "-o", binary,
    ], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=20)
    reference = str(Path(directory) / "vector_offset_linux_reference")
    subprocess.run([
        "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "tools/tests/vector_offset_linux_reference.c", "-o", reference,
    ], cwd=ROOT, check=True)
    subprocess.run([reference], cwd=ROOT, check=True, timeout=20)
