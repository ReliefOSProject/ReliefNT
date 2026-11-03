#!/usr/bin/env python3
"""Run production-linked low-memory fault regression controls."""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    """Record one compiler or scenario command and propagate its status."""
    print("+ " + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False)
    print("exit=" + str(result.returncode), flush=True)
    return result.returncode


def run_cases(output):
    """Run the controls in an already owned output directory."""
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("clang is required")
    binary = str(output / "fault_fix5")
    common = [compiler, "-std=c11", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
              "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
              "-ffunction-sections", "-fdata-sections", "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie",
              "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi"]
    sources = ["tools/tests/memory_pressure_fix5_test.c", "kernel/sched/core.c",
               "kernel/sched/wait.c", "kernel/sched/oom.c", "kernel/sync/wait.c",
               "mm/reclaim/pressure.c", "mm/reclaim/wait.c", "mm/cache/page_cache.c",
               "fs/vfs/mount_tree.c", "fs/vfs/lookup_cache.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c",
               "fs/vfs/exec_file.c", "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c",
               "-Wl,--wrap=vfs_exec_source_get", "-Wl,--wrap=vfs_exec_source_put", "-o", binary]
    status = run([*common, *sources])
    if status:
        return status
    failures = 0
    for case in ("cache-frame", "provider-memory", "provider-large", "anonymous-boundary", "negative", "no-victim",
                 "low-free-jitter", "persistent-allocation", "transient-allocation", "cache-refresh"):
        failures += bool(run([binary, case]))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        directory = nullcontext(args.output.resolve())
    else:
        directory = tempfile.TemporaryDirectory(prefix="memory-pressure-fix5-", dir=os.environ.get("TMPDIR"))
    with directory as workdir:
        return run_cases(Path(workdir))


if __name__ == "__main__":
    raise SystemExit(main())
