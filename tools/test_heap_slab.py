#!/usr/bin/env python3
"""Exercise the actual kernel heap against a bounded, concurrent page provider."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ("density", "classes", "reuse", "invalid", "large", "exhaustion", "mapping",
         "publication", "publication_enomem", "duplicate", "trees", "concurrency")
with tempfile.TemporaryDirectory(prefix="reliefnt-heap-") as work:
    compiler = shutil.which("clang") or "cc"
    sources = ["tools/tests/heap_slab_test.c", "mm/heap/heap.c", "mm/heap/slab.c",
               "mm/heap/tree.c"]
    failed = False
    for variant in ("native", "checked-clearing"):
        binary = Path(work) / f"heap_slab_test-{variant}"
        instrumentation = (["-DHEAP_CHECK_CLEAR", "-D__builtin_memset=fixture_checked_memset",
                            "-include", "tools/tests/heap_checked_memset.h"]
                           if variant == "checked-clearing" else [])
        subprocess.run([compiler, "-std=c11", "-O1", "-g", "-pthread", "-Wall", "-Wextra",
                        "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                        *instrumentation, *sources, "-o", str(binary)], cwd=ROOT, check=True)
        for case in (sys.argv[1:] or CASES):
            print(f"VARIANT {variant} CASE {case}", flush=True)
            failed |= subprocess.run([str(binary), case], cwd=ROOT, timeout=90,
                                     env=os.environ.copy()).returncode != 0
    if failed:
        raise SystemExit(1)
