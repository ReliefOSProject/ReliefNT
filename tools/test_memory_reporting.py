#!/usr/bin/env python3
"""Run production-linked memory-reporting and fault-pressure controls."""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAULT_CASES = ("cache-prefix", "cache-interleaved", "budget-cold", "budget-refresh", "budget-full", "retry-log", "backing-log", "actual-events", "dirty-cache", "dirty-policy", "prefault-shortage", "usercopy-shortage", "ring0-log", "prefault-terminal", "stale-shared", "stale-supervisor", "stale-negative")
PROC_CASES = ("proc-empty-swaps", "proc-meminfo", "proc-vmstat", "proc-oom")
RESOURCE_CASES = ("sysinfo", "swap-stream", "oom-events")
CASES = FAULT_CASES + PROC_CASES + RESOURCE_CASES


def run(argv):
    """Log a complete command, preserve output, and return its status."""
    print("+ " + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False, timeout=60)
    print("exit=" + str(result.returncode), flush=True)
    return result.returncode


def run_cases(output, cases):
    """Compile the linked production paths once and run each selected control."""
    clang = shutil.which("clang")
    nm = shutil.which("nm")
    if not clang or not nm:
        raise RuntimeError("clang and nm are required")
    failures = 0
    if "oom-events" in cases:
        binary = str(output / "memory_reporting_oom_events")
        command = [clang, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-fno-pie", "-no-pie", "-ffunction-sections",
                   "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                   "tools/tests/memory_pressure_oom_test.c", "kernel/sched/identity.c",
                   "kernel/sched/oom.c", "mm/reclaim/oom.c", "mm/reclaim/stats.c", "-o", binary]
        if run(command):
            return 1
        failures += sum(bool(run([binary, case])) for case in ("selection", "protected"))
    if "swap-stream" in cases:
        binary = str(output / "memory_reporting_swap")
        sources = ["tools/tests/memory_reporting_swap_test.c", "fs/proc/memory.c",
                   "mm/swap/area.c", "mm/swap/slot.c", "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c",
                   "fs/vfs/path.c", "fs/vfs/dentry.c", "fs/vfs/file.c", "fs/vfs/open.c",
                   "fs/vfs/pseudo.c", "fs/vfs/attributes.c", "kernel/module/lifecycle.c",
                   "kernel/params/value.c", "kernel/module/layout.c", "kernel/module/symbols.c"]
        command = [clang, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-DCONFIG_MODULES=1",
                   "-DCONFIG_MODULE_UNLOAD=1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                   "-Iinclude", "-Iinclude/uapi", *sources, "-o", binary]
        if run(command):
            return 1
        failures += bool(run([binary]))
    fault_cases = [case for case in cases if case in FAULT_CASES]
    proc_cases = [case for case in cases if case in PROC_CASES]
    if "sysinfo" in cases:
        binary = str(output / "memory_reporting_sysinfo")
        command = [clang, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-fno-pie", "-no-pie", "-ffunction-sections",
                   "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                   "tools/tests/task8_fixed_user_test.c", "-o", binary]
        if run(command):
            return 1
        failures += bool(run([binary]))
    if proc_cases:
        binary = str(output / "memory_reporting_proc")
        proc_sources = ["tools/tests/memory_reporting_proc_test.c", "fs/vfs/mount_tree.c",
                        "fs/vfs/mount_table.c", "fs/vfs/path.c", "fs/vfs/file.c", "fs/vfs/open.c",
                        "fs/vfs/pseudo.c", "fs/vfs/attributes.c", "fs/proc/mount.c",
                        "kernel/sched/identity.c", "kernel/sched/oom.c", "fs/security/access.c",
                        "fs/proc/memory.c", "mm/reclaim/stats.c", "mm/reclaim/oom.c"]
        proc_command = [clang, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                        "-fno-pie", "-no-pie", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                        *proc_sources, "-o", binary]
        if run(proc_command):
            return 1
        failures += sum(bool(run([binary, case])) for case in proc_cases)
    if not fault_cases:
        return 1 if failures else 0
    binary = str(output / "memory_reporting")
    common = [clang, "-std=c11", "-O0", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
              "-finstrument-functions", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
              "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections", "-DCONFIG_SWAP=1", "-DRELIEFNT_CACHE_PROBE=1", "-DRELIEFNT_DIRTY_PROBE=1",
              "-fno-pie", "-no-pie", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi"]
    sources = ["tools/tests/memory_reporting_test.c", "kernel/sched/core.c", "kernel/sched/wait.c",
               "kernel/sched/oom.c", "kernel/sync/wait.c", "mm/reclaim/pressure.c", "mm/reclaim/wait.c",
               "mm/cache/page_cache.c", "mm/reclaim/stats.c", "kernel/exec/usercopy.c", "kernel/exec/usercopy_task.c", "fs/vfs/mount_tree.c", "fs/vfs/file.c", "fs/vfs/open.c",
               "fs/vfs/attributes.c", "fs/vfs/exec_file.c", "mm/swap/area.c", "mm/swap/slot.c",
               "mm/swap/paging.c", "-Wl,--wrap=vfs_exec_source_get", "-Wl,--wrap=vfs_exec_source_put",
               "-o", binary]
    if run([*common, *sources]):
        return 1
    print("+ " + shlex.join([nm, "-an", binary]) + " (captured)", flush=True)
    symbols = subprocess.run([nm, "-an", binary], cwd=ROOT, check=True, capture_output=True, text=True)
    match = re.search(r"^([0-9a-fA-F]+)\s+[tT]\s+reclaimable$", symbols.stdout, re.M)
    if not match:
        raise RuntimeError("cache eligibility predicate missing")
    print("reclaimable=" + match.group(1), flush=True)
    failures += sum(bool(run([binary, case, match.group(1)])) for case in fault_cases)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", action="append", choices=CASES)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    output = args.output or (Path(os.environ["RELIEFNT_TEST_OUT"]) if os.environ.get("RELIEFNT_TEST_OUT") else None)
    if output:
        output.mkdir(parents=True, exist_ok=False)
        directory = nullcontext(output.resolve())
    else:
        directory = tempfile.TemporaryDirectory(prefix="memory-reporting-", dir=os.environ.get("TMPDIR"))
    with directory as workdir:
        return run_cases(Path(workdir), args.case or CASES)


if __name__ == "__main__":
    raise SystemExit(main())
