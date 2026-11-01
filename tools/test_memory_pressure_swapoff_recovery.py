#!/usr/bin/env python3
"""Compile and run the production-linked failed swapoff recovery fixture."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    """Record an exact compiler or fixture invocation and propagate status."""
    print("+ " + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False)
    print("exit=" + str(result.returncode), flush=True)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    else:
        args.output = Path(tempfile.mkdtemp(prefix="swapoff-recovery-", dir=os.environ.get("TMPDIR")))
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("clang is required")
    binary = str(args.output.resolve() / "swapoff_recovery")
    command = [compiler, "-std=c11", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        "-ffunction-sections", "-fdata-sections", "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie",
        "-Wl,--gc-sections,--wrap=swap_drain_step", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/memory_pressure_swapoff_recovery_test.c", "mm/user/context.c", "mm/user/vma.c",
        "kernel/syscall/memory/swap.c", "kernel/syscall/memory/swap_area.c", "mm/reclaim/pressure.c",
        "mm/swap/area.c", "mm/swap/backend.c", "mm/swap/slot.c", "mm/swap/drain.c",
        "mm/swap/paging.c", "kernel/signal/signal_queue.c", "-o", binary]
    status = run(command)
    if status:
        return status
    failures = sum(bool(run([binary, case])) for case in
                   ("partial", "success", "foreign", "io-error", "caller-change",
                    "drain-caller-change", "fatal-before-recovery", "fatal-after-checkpoint"))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
