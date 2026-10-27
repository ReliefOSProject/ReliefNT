#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-inet-") as work:
    for name in ("udp_socket", "tcp_state"):
        binary = Path(work) / name
        subprocess.run(["cc", "-std=c11", "-O1", "-g", "-DRELIEFNT_HOST_TEST",
                        "-fsanitize=address,undefined",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                        f"tools/tests/{name}_test.c", "fs/vfs/mount_tree.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/initial.c", "kernel/syscall/ipc/unix_name.c", "kernel/syscall/fs/lookup_state.c",
                        "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
