#!/usr/bin/env python3
"""Exercise real FAT/exFAT rename, metadata eviction and cache retirement."""
from pathlib import Path
from storage_graph_test_sources import STORAGE_GRAPH_SOURCES
import os
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cache-clustered-",dir=os.environ["TMPDIR"]) as directory:
    work=Path(directory); binary=work/"clustered"
    subprocess.run(["clang","-std=c11","-DRELIEFNT_FIXTURE_REAL_DAC","-O1","-g",
        "-fsanitize=address,undefined","-mllvm","-asan-globals=0","-fno-sanitize-recover=all",
        "-no-pie","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
        "-Iinclude","-Iinclude/uapi","tools/tests/cache_clustered_lifetime_test.c",
        "mm/cache/page_cache.c","fs/tmpfs/core.c","fs/vfs/exec_file.c","lib/text_utf16.c",
        *STORAGE_GRAPH_SOURCES,"-o",str(binary)],cwd=ROOT,check=True)
    env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0")
    failed=False
    for fs in ((sys.argv[1],) if len(sys.argv)>1 else ("fat","exfat")):
        image=work/(fs+".img")
        with image.open("wb") as stream: stream.truncate(128*1024*1024)
        args=["mkfs.fat","--invariant","-F","32","-s","1",str(image)] if fs=="fat" else ["mkfs.exfat",str(image)]
        subprocess.run(args,check=True,stdout=subprocess.DEVNULL)
        for case in ((sys.argv[2],) if len(sys.argv)>2 else
                     ("rename","eviction","delete","held-delete","replace","held-replace","growth","purge","teardown","teardown-fail")):
            result=subprocess.run([str(binary),str(image),fs,case],cwd=ROOT,env=env,timeout=45)
            print(f"{fs} {case}: exit {result.returncode}",flush=True)
            failed |= result.returncode!=0
    if failed: sys.exit(1)
