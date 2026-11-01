#!/usr/bin/env python3
"""Exercise ext punch progress using the real provider and injected disk I/O."""
from pathlib import Path
from storage_graph_test_sources import STORAGE_GRAPH_SOURCES
import os
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cache-ext-partial-",dir=os.environ["TMPDIR"]) as directory:
    work=Path(directory); binary=work/"partial"; image=work/"ext.img"; data=work/"data"
    subprocess.run(["clang","-std=c11","-DRELIEFNT_FIXTURE_REAL_DAC","-O1","-g",
        "-fsanitize=address,undefined","-mllvm","-asan-globals=0","-fno-sanitize-recover=all",
        "-no-pie","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
        "-Iinclude","-Iinclude/uapi","tools/tests/cache_ext_partial_test.c",
        "mm/cache/page_cache.c","fs/tmpfs/core.c","fs/vfs/exec_file.c","lib/text_utf16.c",
        *STORAGE_GRAPH_SOURCES,"-o",str(binary)],cwd=ROOT,check=True)
    with image.open("wb") as stream: stream.truncate(64*1024*1024)
    subprocess.run(["mke2fs","-q","-F","-t","ext2","-b","4096","-I","256",
        "-O","none,filetype,large_file,dir_index",str(image)],check=True)
    data.write_bytes(bytes([0x5a])*8192)
    subprocess.run(["debugfs","-w","-R",f"write {data} /payload",str(image)],check=True,
                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    failed=False
    for case in ((sys.argv[1],) if len(sys.argv)>1 else
                 ("clean","dirty","clean-zero","dirty-zero","aligned","resize",
                  "growth-clean","growth-dirty","growth-dirty-loader",
                  "growth-dirty-independent-stat","growth-dirty-independent-lookup")):
        result=subprocess.run([str(binary),str(image),case],cwd=ROOT,
                              env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"),timeout=45)
        print(f"{case}: exit {result.returncode}",flush=True); failed |= result.returncode!=0
    if failed: sys.exit(1)
