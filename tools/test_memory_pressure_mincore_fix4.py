#!/usr/bin/env python3
"""Exercise real syscall entry and paging during a mincore output shortage."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

from test_user_return_gate import function

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    print("+ " + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False, timeout=120)
    print("exit=" + str(result.returncode), flush=True)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", action="append", choices=("lazy", "recovery", "terminal",
                        "contention", "no-victim", "persistent", "query-revalidate",
                        "output-revalidate", "cancel"))
    args = parser.parse_args()
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("clang is required")
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        directory = args.output.resolve()
    else:
        directory = Path(tempfile.mkdtemp(prefix="memory-mincore-", dir=os.environ.get("TMPDIR")))
    (directory / "mincore_syscall_entry.inc").write_text(
        function(ROOT / "kernel/syscall/entry.c", "syscall_dispatch_frame"))
    binary = str(directory / "memory_mincore")
    common = [compiler, "-std=c11", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
              "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
              "-ffunction-sections", "-fdata-sections", "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie",
              "-Wl,--gc-sections", "-I" + str(directory), "-Iinclude", "-Iinclude/uapi"]
    sources = ["tools/tests/memory_pressure_mincore_fix4_test.c", "kernel/exec/usercopy.c",
               "kernel/syscall/fs/lookup_state.c",
               "kernel/sched/core.c", "kernel/sched/wait.c", "kernel/sched/oom.c", "kernel/sync/wait.c",
               "mm/reclaim/pressure.c", "mm/reclaim/wait.c", "mm/cache/page_cache.c",
               "fs/vfs/mount_tree.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c",
               "fs/vfs/exec_file.c", "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c",
               "-Wl,--wrap=vfs_exec_source_get", "-Wl,--wrap=vfs_exec_source_put",
               "-Wl,--wrap=swap_backend_page_io", "-o", binary]
    status = run([*common, *sources])
    if status:
        return status
    failures = 0
    for case in args.case or ("lazy", "recovery", "terminal", "contention", "no-victim", "persistent", "query-revalidate", "output-revalidate", "cancel"):
        if args.case:
            print("partial case=" + case, flush=True)
        failures += bool(run([binary, case]))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
