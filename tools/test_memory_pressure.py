#!/usr/bin/env python3
"""Exercise production pressure admission with retained host evidence."""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    """Run and log one exact compiler or fixture command."""
    print("+ " + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False)
    print("exit=" + str(result.returncode), flush=True)
    result.check_returncode()


def run_held_probe(binary):
    """Exercise the probe's live-MM hold and release on this host only."""
    argv = [str(binary), "1048576", "--hold", "--thread", "--oom-adjust", "500"]
    print("+ " + shlex.join(argv), flush=True)
    process = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    captured = bytearray()
    try:
        deadline = time.monotonic() + 30
        while b"PRESSURE_READY " not in captured:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([process.stdout], [], [], remaining)[0]:
                raise TimeoutError("probe did not reach live-MM hold")
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                raise RuntimeError("probe exited before its live-MM hold")
            captured.extend(chunk)
        print("+ signal SIGUSR1 to retained probe pid=" + str(process.pid), flush=True)
        os.kill(process.pid, signal.SIGUSR1)
        tail, _ = process.communicate(timeout=30)
        captured.extend(tail)
        print(captured.decode(errors="replace"), end="", flush=True)
        print("exit=" + str(process.returncode), flush=True)
        if process.returncode or any(marker not in captured for marker in
              (b"PRESSURE_OOM_ADJ requested=500 observed=500", b"PRESSURE_THREAD ready=1",
               b"PRESSURE_READY ", b"PRESSURE_RESUMED", b"PRESSURE_THREAD joined=1", b"PRESSURE_PASS")):
            raise RuntimeError("held probe did not complete its real host control")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="New directory for retained objects and binaries")
    args = parser.parse_args()
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("clang is required for sanitizer fixtures")
    output = args.output or (Path(os.environ["RELIEFNT_TEST_OUT"]) if os.environ.get("RELIEFNT_TEST_OUT") else None)
    if output is not None:
        output = output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        workdir = nullcontext(output)
    else:
        workdir = tempfile.TemporaryDirectory(prefix="memory-pressure-", dir=os.environ.get("TMPDIR"))
    with workdir as directory:
        work = Path(directory)
        common = [compiler, "-std=c11", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
                  "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                  "-ffunction-sections", "-fdata-sections", "-Iinclude", "-Iinclude/uapi"]
        heap = work / "heap.o"
        binary = work / "physical_reserve"
        run([*common, "-Dmm_virt_to_phys=fixture_heap_virt_to_phys", "-c",
             "mm/heap/heap.c", "-o", str(heap)])
        run([*common, "-Wl,--gc-sections", "tools/tests/memory_pressure_pmm_test.c",
             "mm/physical/mm.c", "mm/physical/metadata.c", "mm/physical/topology.c",
             "mm/physical/bootstrap.c", "arch/x86_64/boot/multiboot2.c", str(heap),
             "mm/heap/slab.c", "mm/heap/tree.c", "-o", str(binary)])
        for case in ("ordinary", "small"):
            run([str(binary), case])
        cache = work / "cache_budget"
        run([*common, "-O0", "-finstrument-functions", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_cache_test.c", "mm/cache/page_cache.c",
             "kernel/sync/lock.c", "-o", str(cache)])
        nm = shutil.which("nm")
        if not nm:
            raise RuntimeError("nm is required to identify the cache predicate")
        symbols = subprocess.run([nm, "-an", str(cache)], cwd=ROOT, check=True,
                                 stdout=subprocess.PIPE, text=True).stdout
        match = re.search(r"^([0-9a-fA-F]+)\s+[tT]\s+reclaimable$", symbols, re.M)
        if not match:
            raise RuntimeError("cache predicate symbol was not found")
        print("+ " + shlex.join([nm, "-an", str(cache)]) + " (captured)", flush=True)
        print("reclaimable=" + match.group(1), flush=True)
        for case in ("normal", "pinned", "overflow"):
            run([str(cache), match.group(1), case])
        drain = work / "drain"
        run([*common, "-DCONFIG_SWAP=1", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_drain_test.c", "mm/swap/area.c", "mm/swap/slot.c",
             "-o", str(drain)])
        run([str(drain)])
        restore = work / "restore"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_restore_test.c", "kernel/exec/usercopy.c",
             "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c", "-o", str(restore)])
        run([str(restore)])
        swapoff = work / "swapoff"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_swapoff_test.c", "kernel/exec/usercopy.c",
             "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/drain.c", "mm/swap/paging.c", "-o", str(swapoff)])
        run([str(swapoff)])
        swapoff_front = work / "swapoff_frontdoor"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_swapoff_frontdoor_test.c",
             "mm/user/context.c", "mm/user/vma.c", "kernel/syscall/memory/swap.c", "kernel/syscall/memory/swap_area.c",
             "mm/swap/area.c", "mm/swap/backend.c", "mm/swap/slot.c", "mm/swap/drain.c",
             "mm/swap/paging.c", "-o", str(swapoff_front)])
        run([str(swapoff_front)])
        scan = work / "sparse_scan"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_scan_test.c", "kernel/exec/usercopy.c",
             "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c", "-o", str(scan)])
        run([str(scan)])
        metadata = work / "metadata"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_metadata_test.c", "kernel/exec/usercopy.c",
             "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c", "-o", str(metadata)])
        run([str(metadata)])
        engine = work / "pressure_engine"
        engine_sources = ["tools/tests/memory_pressure_engine_test.c"]
        if (ROOT / "mm/reclaim/pressure.c").exists():
            engine_sources.append("mm/reclaim/pressure.c")
        run([*common, "-DCONFIG_SWAP=1", "-no-pie", "-Wl,--gc-sections",
             *engine_sources, "-o", str(engine)])
        run([str(engine)])
        cross = work / "cross_pte"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_cross_pte_test.c", "kernel/exec/usercopy.c",
             "kernel/sched/oom.c", "mm/reclaim/pressure.c", "mm/swap/area.c",
             "mm/swap/slot.c", "mm/swap/paging.c", "-o", str(cross)])
        run([str(cross)])
        timer = work / "timer_entry"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_timer_entry_test.c", "-o", str(timer)])
        run([str(timer)])
        irq = work / "irq_entry"
        run([*common, "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_irq_entry_test.c", "-o", str(irq)])
        run([str(irq)])
        oom = work / "oom"
        run([*common, "-DCONFIG_SWAP=1", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_oom_test.c", "kernel/sched/identity.c",
             "kernel/sched/oom.c", "mm/reclaim/oom.c", "mm/reclaim/stats.c", "-o", str(oom)])
        for case in ("policy", "selection", "protected"):
            run([str(oom), case])
        oom_lifecycle = work / "oom_lifecycle"
        run([*common, "-Wl,--gc-sections", "tools/tests/memory_pressure_oom_lifecycle_test.c",
             "kernel/signal/signal_queue.c", "kernel/sched/oom.c", "mm/reclaim/oom.c",
             "-o", str(oom_lifecycle)])
        run([str(oom_lifecycle)])
        fault = work / "fault_pressure"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_fault_test.c", "kernel/sched/core.c",
             "mm/cache/page_cache.c", "fs/vfs/mount_tree.c", "fs/vfs/lookup_cache.c", "fs/vfs/file.c",
             "fs/vfs/open.c", "fs/vfs/attributes.c", "fs/vfs/exec_file.c",
             "mm/swap/area.c", "mm/swap/slot.c", "mm/swap/paging.c",
             "-Wl,--wrap=vfs_exec_source_get", "-Wl,--wrap=vfs_exec_source_put", "-o", str(fault)])
        run([str(fault)])
        integration = work / "fault_integration"
        run([*common, "-DCONFIG_SWAP=1", "-fno-pie", "-no-pie", "-Wl,--gc-sections",
             "tools/tests/memory_pressure_fault_integration_test.c", "kernel/sched/core.c",
             "kernel/sched/wait.c", "kernel/sched/oom.c", "kernel/sync/wait.c",
             "mm/reclaim/pressure.c", "mm/reclaim/wait.c", "mm/cache/page_cache.c",
             "fs/vfs/mount_tree.c", "fs/vfs/lookup_cache.c", "fs/vfs/file.c", "fs/vfs/open.c",
             "fs/vfs/attributes.c", "fs/vfs/exec_file.c", "mm/swap/area.c",
             "mm/swap/slot.c", "mm/swap/paging.c", "-Wl,--wrap=vfs_exec_source_get",
             "-Wl,--wrap=vfs_exec_source_put", "-o", str(integration)])
        run([str(integration)])
        waiting = work / "pressure_wait"
        run([*common, "-Wl,--gc-sections", "tools/tests/memory_pressure_wait_test.c",
             "kernel/sync/wait.c", "kernel/sched/oom.c", "mm/reclaim/wait.c", "-o", str(waiting)])
        run([str(waiting)])
        probe = work / "memory_pressure_guest_probe"
        run([*common, "tools/tests/memory_pressure_guest_probe.c", "-o", str(probe)])
        run([str(probe), "1048576"])
        run_held_probe(probe)


if __name__ == "__main__":
    main()
