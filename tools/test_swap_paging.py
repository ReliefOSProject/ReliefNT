#!/usr/bin/env python3
"""Exercise swap leaf ownership, trap admission, and scheduler wait behavior."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
compiler = shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("a C compiler is required")

common = [compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
          "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-ffunction-sections",
          "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi"]
fixtures = [
    ("paging", ["tools/tests/swap_paging_test.c", "kernel/exec/usercopy.c",
                 "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c"]),
    ("fault-gate", ["-pthread", "tools/tests/swap_fault_gate_test.c", "kernel/sched/core.c",
                     "mm/cache/page_cache.c", "fs/vfs/mount_tree.c", "fs/vfs/file.c",
                     "fs/vfs/open.c", "fs/vfs/attributes.c", "fs/vfs/exec_file.c",
                     "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c",
                     "-Wl,--wrap=vfs_exec_source_get", "-Wl,--wrap=vfs_exec_source_put"]),
    ("scheduler-wait", ["tools/tests/swap_wait_scheduler_test.c"]),
]
with tempfile.TemporaryDirectory(prefix="reliefnt-swap-paging-", dir=os.environ.get("TMPDIR")) as work:
    for name, sources in fixtures:
        binary = str(Path(work) / name)
        subprocess.run([*common, *sources, "-o", binary], cwd=ROOT, check=True)
        subprocess.run([binary], cwd=ROOT, check=True, timeout=30)
