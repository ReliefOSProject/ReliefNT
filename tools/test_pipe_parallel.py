#!/usr/bin/env python3
"""Exercise pipe transactions with real SMP locks and controlled user pages."""
from pathlib import Path
import os
import subprocess
import tempfile
from kernel_test_sources import module_text
from test_syscall_io import function

ROOT = Path(__file__).resolve().parents[1]
source = module_text("syscall_dispatch")
temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="pipe-parallel-", dir=temporary) as directory:
    work = Path(directory)
    if "static bool syscall_try_pipe_io(" in source:
        bodies = [function(source, name) for name in (
            "syscall_trace_task", "syscall_pipe_buffer_resident", "syscall_try_pipe_io")]
    else:
        # Old kernels route every pipe transaction through the exclusive gate.
        bodies = ["static bool syscall_try_pipe_io(struct trap_frame *f, struct task **r) "
                  "{ (void)f; (void)r; return false; }"]
    (work / "pipe_entry.inc").write_text("\n\n".join(bodies))
    binary = work / "pipes"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-pthread", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi", "-I" + str(work),
        "tools/tests/pipe_parallel_test.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=30)
    paging = (ROOT / "arch/x86_64/mm/paging.c").read_text()
    (work / "pipe_pages.inc").write_text("\n\n".join(function(paging, name) for name in (
        "table_from_entry", "user_address_valid", "address_space_pte",
        "address_space_user_page_flags", "address_space_user_write_branches",
        "address_space_user_page_copyable")))
    page_binary = work / "pages"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
        "-I" + str(work), "tools/tests/pipe_page_test.c", "-o", str(page_binary)], cwd=ROOT, check=True)
    subprocess.run([page_binary], cwd=ROOT, check=True, timeout=10)
