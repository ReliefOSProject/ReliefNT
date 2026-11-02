#!/usr/bin/env python3
"""Focused scalar regular-read reclaim integration fixture."""
import argparse
from pathlib import Path
import shlex
import shutil
import subprocess

from test_user_return_gate import function

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    print("+ " + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False, timeout=120)
    print("exit=" + str(result.returncode), flush=True)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("clang is required")
    args.output.mkdir(parents=True, exist_ok=False)
    directory = args.output.resolve()
    (directory / "mincore_syscall_entry.inc").write_text(
        function(ROOT / "kernel/syscall/entry.c", "syscall_dispatch_frame"))
    (directory / "regular_release.inc").write_text(
        function(ROOT / "kernel/syscall/fd/descriptor.c", "task_release_syscall_file"))
    binary = str(directory / "memory_regular_read")
    common = [compiler, "-std=c11", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
              "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
              "-ffunction-sections", "-fdata-sections", "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie",
              "-Wl,--gc-sections", "-I" + str(directory), "-Iinclude", "-Iinclude/uapi"]
    sources = ["tools/tests/memory_pressure_regular_read_test.c", "kernel/exec/usercopy.c",
               "tools/tests/task8_host_stubs/device_page_unreached.c",
               "kernel/syscall/fs/lookup_state.c", "kernel/sched/core.c", "kernel/sched/accounting.c",
               "kernel/sched/wait.c",
               "kernel/sched/oom.c", "kernel/sync/wait.c", "mm/reclaim/pressure.c",
               "mm/reclaim/wait.c", "mm/cache/page_cache.c", "fs/vfs/mount_tree.c",
               "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c", "fs/vfs/exec_file.c",
               "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c",
               "-Wl,--wrap=vfs_exec_source_get", "-Wl,--wrap=vfs_exec_source_put",
               "-Wl,--wrap=swap_backend_page_io", "-o", binary]
    status = run([*common, *sources])
    return status or run([binary])


if __name__ == "__main__":
    raise SystemExit(main())
