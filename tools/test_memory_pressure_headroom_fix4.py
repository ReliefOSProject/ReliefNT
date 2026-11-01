#!/usr/bin/env python3
"""Run the production-linked proactive low-memory fault controls."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def run(argv):
    print("+ "+shlex.join(argv),flush=True)
    result=subprocess.run(argv,cwd=ROOT,check=False,timeout=120)
    print("exit="+str(result.returncode),flush=True)
    return result.returncode


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    compiler=shutil.which("clang")
    if not compiler:
        raise RuntimeError("clang is required")
    if args.output:
        args.output.mkdir(parents=True,exist_ok=False)
        out=args.output.resolve()
    else:
        out=Path(tempfile.mkdtemp(prefix="memory-headroom-",dir=os.environ.get("TMPDIR")))
    binary=str(out/"headroom")
    common=[compiler,"-std=c11","-O1","-g","-pthread","-Wall","-Wextra","-Werror",
            "-fsanitize=address,undefined","-fno-sanitize-recover=all","-fno-omit-frame-pointer",
            "-ffunction-sections","-fdata-sections","-DCONFIG_SWAP=1","-fno-pie","-no-pie",
            "-Wl,--gc-sections","-Iinclude","-Iinclude/uapi"]
    sources=["tools/tests/memory_pressure_headroom_fix4_test.c","kernel/sched/core.c","kernel/sched/exec.c",
             "kernel/sched/wait.c","kernel/sched/oom.c","kernel/sync/wait.c",
             "mm/reclaim/pressure.c","mm/reclaim/wait.c","mm/cache/page_cache.c",
             "fs/vfs/mount_tree.c","fs/vfs/file.c","fs/vfs/open.c","fs/vfs/attributes.c",
             "fs/vfs/exec_file.c","mm/swap/area.c","mm/swap/slot.c","mm/swap/paging.c",
             "-Wl,--wrap=vfs_exec_source_get","-Wl,--wrap=vfs_exec_source_put",
             "-Wl,--wrap=swap_backend_page_io","-Wl,--wrap=mm_alloc_page",
             "-Wl,--wrap=paging_load_cr3","-o",binary]
    status=run([*common,*sources])
    if status:
        return status
    failures=0
    for case in ("early","above-low","partial","swapon","high-recovery","protection","cancel","shared-change","fault-priority","first-generation-priority","absent-generation-priority","promoted-generation-change","pending-generation","pending-root","pending-fault","pending-root-zero","pending-fault-zero","owned-wake","owned-cancel","exec-pending","exec-unrelated","exec-pending-other","exec-pressure-unrelated","exec-pending-pressure","root-change","write-eio","write-contended"):
        failures+=bool(run([binary,case]))
    return 1 if failures else 0


if __name__=="__main__":
    raise SystemExit(main())
