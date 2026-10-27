#!/usr/bin/env python3
"""Test production fd timestamp handling and its Linux syscall reference."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TMP = Path(os.environ.get("TMPDIR", str(ROOT / "out" / "tmp")))
TMP.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='futimens-', dir=TMP) as directory:
    for name, flags in (
        ('futimens_test', ['-O1', '-g', '-ffunction-sections', '-fdata-sections',
                          '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                          '-I.', '-Iinclude', '-Iinclude/uapi',
                          '-Iinclude', '-Wl,--gc-sections']),
        ('futimens_runtime_probe', ['-O2', '-Wall', '-Wextra', '-Werror']),
    ):
        binary = Path(directory) / name
        sources = []
        if name == 'futimens_test':
            sources = [
                'kernel/syscall/fs/filesystem.c', 'kernel/syscall/fs/timestamps.c', 'fs/vfs/mount_tree.c', 'fs/vfs/path.c',
                'fs/vfs/attributes.c', 'fs/vfs/file.c', 'fs/vfs/initial.c', 'fs/tmpfs/core.c',
                'fs/security/access.c', 'kernel/syscall/fs/path.c', 'kernel/syscall/fs/lookup_state.c',
                'kernel/syscall/fd/descriptor.c', 'kernel/device/character.c', 'kernel/syscall/io/vector.c', 'kernel/syscall/fd/pty_descriptor.c',
                'kernel/sched/tables.c', 'kernel/device/mapping.c', 'kernel/sched/fs.c',
            ]
        subprocess.run(['clang', '-std=c11', *flags, f'tools/tests/{name}.c', *sources, '-o', str(binary)],
                       cwd=ROOT, env={**os.environ, 'TMPDIR': str(TMP)}, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
